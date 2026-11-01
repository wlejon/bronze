// Method-call dispatch for flow analysis: which class methods a `recv.m(...)`
// site can reach, and the contribution its arguments make to their parameters.
// Split from flow_expr.cpp, whose expression rules call into it.
//
// THE OBLIGATION. A method's parameter types are a join over every call site
// that can reach it, and the body is lowered on that join (`x + 1` on a
// parameter typed `number` is an f64 add after an unbox). So a site may leave a
// method out of its contribution only when it is SURE the call cannot reach it.
// A receiver whose class is a fact bounds the dispatch to that class and the
// overrides below it; any other receiver contributes to every method of the
// name (the unbounded path), whose cost is precision and never a wrong type.

#include <cstdlib>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "types/flow_analyzer.h"

namespace bronze::types {
namespace {

// This site's contribution to what class parameter `i` receives.
//
// Only an argument that NAMES a class says anything: a `Dynamic` argument is
// the absence of an observation, not an observation of absence, and the guard
// this feeds decides the question at run time for whatever the argument
// actually is. Two callers naming different classes leave an object with no
// class, which is how "the callers disagree" is spelled, and no claim is made.
void contributeShape(ModuleContext& mod, uint32_t index, const MethodInfo& target, size_t i,
                     const std::vector<Type>& args) {
    if (i >= args.size() || i >= target.observedParamShapes.size()) return;
    if (!args[i].is(TypeKind::Object) || args[i].shapeClass() == kNoShapeClass) return;
    mod.contribute(InferMemo::Contrib::MethodShape, index, static_cast<uint32_t>(i),
                   Type::object(args[i].shapeClass()));
}

// A parameter's contribution to a method's join, under pin optimism's rule for
// a Dynamic argument (see `methodCall`).
void contributeMethodArg(ModuleContext& mod, uint32_t index, const MethodInfo& target, size_t i,
                         const std::vector<Type>& args) {
    const auto param = static_cast<uint32_t>(i);
    if (i < args.size()) {
        if (target.hasDefault.size() > i && target.hasDefault[i] && args[i].is(TypeKind::Undefined)) {
            return;
        }
        if (mod.pinOptimism() && args[i].is(TypeKind::Dynamic)) {
            mod.contribute(InferMemo::Contrib::MethodSkipped, index, param, Type::dynamic());
            return;
        }
        mod.contribute(InferMemo::Contrib::MethodParam, index, param, args[i]);
    } else if (target.hasDefault.size() > i && !target.hasDefault[i]) {
        mod.contribute(InferMemo::Contrib::MethodParam, index, param, Type::undefined());
    }
}

bool isLiteralExpr(const ast::Expr* e) {
    return ast::exactAs<const ast::ArrayLit*>(e) || ast::exactAs<const ast::ObjectLit*>(e) ||
           ast::exactAs<const ast::NumberLit*>(e) || ast::exactAs<const ast::StringLit*>(e) ||
           ast::exactAs<const ast::BoolLit*>(e) || ast::exactAs<const ast::RegExpLit*>(e);
}

}  // namespace

// A receiver whose member call can reach no class method at all: a literal that
// is not an object literal (its prototype is a builtin's, which no class method
// can be installed on without a value read of the method, and that poisons it),
// or a builtin namespace global the program does not rebind (`Math.max`,
// `JSON.parse`). An object literal is NOT here: `{ __proto__: v }` dispatches to
// `v`'s chain, so it goes down the unbounded path like any classless object.
bool FlowAnalyzer::receiverNamesNoClass(const ast::Expr* e) const {
    if (e == nullptr) return false;
    if (isLiteralExpr(e)) return ast::exactAs<const ast::ObjectLit*>(e) == nullptr;
    const auto* id = ast::exactAs<const ast::Ident*>(e);
    if (id == nullptr || resolvesToUserBinding(id->name)) return false;
    static const char* kBuiltinNamespaces[] = {
        "Math", "Object", "Array", "Number", "String", "Boolean", "Symbol", "JSON",
        "console", "performance", "Promise", "Date", "Intl", "Atomics",
    };
    for (const char* n : kBuiltinNamespaces) {
        if (id->name == n) return true;
    }
    return false;
}

// Whether `receiver` limits a call to `cls`'s methods and the overrides below
// it. Only facts do:
//   - `super.m()`, whose lookup starts at a base the class text names;
//   - `this` in a method that did not escape (MethodPoison): such a method is
//     reached only by a lookup that found it on the receiver's chain, so the
//     chain holds the class prototype. An escaped method can be `.call`ed on
//     anything;
//   - an identity the walk watched being made (`builtHere`, not a join over
//     callers): `new C()` and the bindings it flowed into.
// A field read, a method's return, an array element or a guessed parameter
// identity is an object a write this walk never saw may have replaced, so it
// bounds nothing. Nor does any class whose `extends` the analysis could not
// resolve: its methods' lookup continues into a chain no table describes.
bool FlowAnalyzer::dispatchBounded(Type receiver, ReceiverForm form, const ClassLayout* cls) {
    // An object's prototype can change after it was made: the program names
    // setPrototypeOf / __proto__ (method_ident.cpp, scanProtoMutation), or a
    // computed write the audit could not prove harmless may write the key
    // "__proto__" through Object.prototype's setter. Either way the class an
    // object was built from no longer says whose method a call reaches.
    if (mod_.protoMutable) return false;
    // Every form: a write that re-parents `this` itself mid-method leaves the
    // next `this.m()` looking up a chain that need not hold the class at all.
    if (InferMemo* memo = mod_.rec()) memo->noteProtoWrites(mod_.fieldAudit.computedWriteRefuted());
    if (mod_.fieldAudit.computedWriteRefuted()) return false;
    for (const ClassLayout* at = cls; at != nullptr;) {
        if (at->superUnknown) return false;
        if (at->superName.empty()) break;
        at = mod_.result->classLayouts.byName(at->superName);
        if (at == nullptr) return false;
    }
    if (form == ReceiverForm::Super) return true;
    if (form == ReceiverForm::This) {
        // A lookup that found this method found it on a chain holding its
        // class's prototype; whatever sits in FRONT of that prototype is the
        // class's own subclasses, unless some class extends a base the table
        // cannot see (a mixin's parameter, a function whose prototype was
        // pointed at this class's) — that class's methods sit there unseen.
        if (mod_.unresolvedBase) return false;
        // A base class's constructor: `this` is the object `new` made from
        // new.target's prototype, and new.target is this class or one below it
        // (Reflect.construct, which could name any other, is scanned for with
        // the prototype mutations).
        if (scope_.thisFromNew) return true;
        if (scope_.thisMethod == kNoMethod) return false;
        const MethodInfo& self = mod_.methods.methods()[scope_.thisMethod];
        const bool poisoned = mod_.methodPoison.poisons(scope_.thisMethod);
        if (InferMemo* memo = mod_.rec()) memo->noteMethod(scope_.thisMethod, self.version, poisoned);
        return !poisoned;
    }
    return receiver.builtHere() && !receiver.identityOnly();
}

// One `recv.name(...)`.
//
// The contribution is the whole mechanism: every method this call can reach
// joins the argument types at this site into its `observedParams`, and
// `widenSignatures` folds those into the signature the method's body is then
// analysed under. Everything else here is the discipline that keeps the join
// from speaking for callers it never saw.
Type FlowAnalyzer::methodCall(const std::string& name, Type receiver,
                              const std::vector<Type>& args, bool spreadArgs, ReceiverForm form) {
    // Nothing has reached this receiver yet. The call-graph fixpoint has not
    // finished; a round that read this as "unproven" would be poisoning the name
    // on evidence that does not exist.
    if (receiver.is(TypeKind::Never)) return Type::never();
    // No class declares a method by this name, so no parameter of any modelled
    // method is at stake — this is a call on a builtin, on host code, or on a
    // function a property happens to hold.
    if (!mod_.methods.isMethodName(name)) return Type::dynamic();

    // A primitive or an array reaches a builtin prototype, which holds a class
    // method only through a value read of it (a poison). An object with no
    // class — two classes joined, a Proxy — or an object literal's shape takes
    // the unbounded path below.
    if (receiver.is(TypeKind::Array) || receiver.is(TypeKind::TypedArray) ||
        receiver.is(TypeKind::Number) || receiver.is(TypeKind::String) ||
        receiver.is(TypeKind::Bool) || receiver.is(TypeKind::Null) ||
        receiver.is(TypeKind::Undefined)) {
        return Type::dynamic();
    }
    if (lastMember_ != nullptr && lastMemberBase_ == receiver &&
        receiverNamesNoClass(lastMember_->object.get())) {
        return Type::dynamic();
    }

    const ClassLayout* cls = receiverClass(receiver);
    if (cls != nullptr && !dispatchBounded(receiver, form, cls)) cls = nullptr;
    if (cls == nullptr) {
        const auto* decls = mod_.methods.declarationsOf(name);
        if (decls == nullptr || decls->empty()) return Type::dynamic();
        if (spreadArgs) {
            for (const uint32_t index : *decls) {
                const MethodInfo& target = mod_.methods.methods()[index];
                const uint32_t cIdx = mod_.ctors.classIndexOf(target.className);
                if (InferMemo* memo = mod_.rec(); memo != nullptr && cIdx != kNoCtor) {
                    memo->noteClassReach(cIdx, mod_.ctors.classReach()[cIdx].version);
                }
                if (!mod_.ctors.isClassReached(target.className)) continue;
                mod_.contribute(InferMemo::Contrib::MethodReached, index, 0, Type::never());
                mod_.methodPoison.add(index, "an argument list is spread at a call site");
            }
            return Type::dynamic();
        }
        auto contributeArgs = [&](uint32_t index, const MethodInfo& target) {
            mod_.contribute(InferMemo::Contrib::MethodReached, index, 0, Type::never());
            for (size_t i = 0; i < target.observedParams.size(); ++i) {
                contributeShape(mod_, index, target, i, args);
                contributeMethodArg(mod_, index, target, i, args);
            }
        };
        Type ret = Type::never();
        bool everyTargetSpeaks = true;
        for (const uint32_t index : *decls) {
            MethodInfo& target = mod_.methods.methods()[index];
            const uint32_t cIdx = mod_.ctors.classIndexOf(target.className);
            if (InferMemo* memo = mod_.rec(); memo != nullptr && cIdx != kNoCtor) {
                memo->noteClassReach(cIdx, mod_.ctors.classReach()[cIdx].version);
            }
            if (!mod_.ctors.isClassReached(target.className)) continue;
            // A target declaring more parameters than this site passes is
            // reached all the same: the call does not pick its method by
            // arity, so each omitted parameter without a default receives
            // `undefined` here (contributeMethodArg). Skipping such a target
            // left its join to the other sites, and a parameter typed `number`
            // from them read `undefined` as NaN.
            contributeArgs(index, target);
            if (InferMemo* memo = mod_.rec()) {
                memo->noteMethod(index, target.version, mod_.methodPoison.poisons(index));
            }
            if (mod_.methodPoison.poisons(index)) {
                everyTargetSpeaks = false;
                continue;
            }
            ret = join(ret, target.signature.returnType);
        }
        if (record_) ++mod_.unboundedMethodCalls;
        // The value, as a GUESS only, on the rungs the bounded path below uses:
        // the receiver may hold a function no class declares (an object
        // literal's own `clone`), which is a guess about whether the value is
        // an instance AT ALL — `objectNotBuiltHere`, whose consumers keep the
        // shape guard and drop every value claim — and never a primitive.
        // `this.children[i].clone()` is the site that pays: its value is what
        // `addChild`'s join hears, and its fields are read behind a cell.
        if (!everyTargetSpeaks) return Type::dynamic();
        if (ret.is(TypeKind::Object) && ret.shapeClass() != kNoShapeClass) {
            return ret.builtHere() && !mod_.result->classLayouts.isExtended(ret.shapeClass())
                       ? Type::objectNotBuiltHere(ret.shapeClass())
                       : Type::objectIdentityOnly(ret.shapeClass());
        }
        return Type::dynamic();
    }
    std::vector<uint32_t> targets;
    mod_.methods.reachableFrom(cls->name, name, targets);
    if (targets.empty()) return Type::dynamic();

    if (spreadArgs) {
        for (const uint32_t index : targets) {
            mod_.contribute(InferMemo::Contrib::MethodReached, index, 0, Type::never());
            mod_.methodPoison.add(index, "an argument list is spread at a call site");
        }
        return Type::dynamic();
    }

    // As in the unbounded-receiver path above — under pin optimism a Dynamic
    // argument does not poison the join, and the parameter is marked so the fold
    // can charge for it.
    Type ret = Type::never();
    bool everyTargetSpeaks = true;
    for (const uint32_t index : targets) {
        const MethodInfo& target = mod_.methods.methods()[index];
        mod_.contribute(InferMemo::Contrib::MethodReached, index, 0, Type::never());
        for (size_t i = 0; i < target.observedParams.size(); ++i) {
            contributeShape(mod_, index, target, i, args);
            contributeMethodArg(mod_, index, target, i, args);
        }
        if (InferMemo* memo = mod_.rec()) {
            memo->noteMethod(index, target.version, mod_.methodPoison.poisons(index));
        }
        if (mod_.methodPoison.poisons(index)) {
            everyTargetSpeaks = false;
            continue;
        }
        ret = join(ret, target.signature.returnType);
    }
    // The value: what every method this dispatch can reach returns. An identity
    // out of here is as good as the dispatch that chose it, which is a guess —
    // so it travels as one. `return this` is the case that pays: three.js
    // methods chain, and `v.copy(u).add(w)` is two sites, not one.
    if (!everyTargetSpeaks) return Type::dynamic();
    if (ret.is(TypeKind::Never)) return Type::never();
    if (ret.is(TypeKind::Object) && ret.shapeClass() != kNoShapeClass) {
        return ret.builtHere() && !mod_.result->classLayouts.isExtended(ret.shapeClass())
                   ? Type::objectNotBuiltHere(ret.shapeClass())
                   : Type::objectIdentityOnly(ret.shapeClass());
    }
    // Only an identity is worth carrying out of an optimistic dispatch. A
    // primitive return type would be a claim about a VALUE, which is what
    // `Type::objectIdentityOnly` exists to keep this mechanism out of.
    return Type::dynamic();
}

}  // namespace bronze::types
