// The expression half of flow analysis: what type an expression produces,
// which callee a call site contributes an argument to, and which shape class
// a literal or a `new` interns. The statement walk is the other half and
// lives in flow.cpp; the seam is argued in flow_analyzer.h.

#include <algorithm>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "types/flow_analyzer.h"
#include "types/operator_types.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

// The ordered property names a constructor installs on `this`, in source order.
// Does not descend into nested functions: each one binds its own receiver, so
// an inner `this.x =` says nothing about this constructor.
//
// Conditional assignments are collected unconditionally, so this can name a
// class the runtime never builds. That is deliberate and safe: the inline-cache
// check keeps the shape guard even on a proven site, because the proof is over
// this compilation's source and the shape word is the runtime's authority.
class ThisPropertyWalker final : public Walker {
public:
    std::vector<std::string> properties;

    void visit(const ast::FunctionExpr&) override {}
    void visit(const ast::FunctionDecl&) override {}

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Assign) {
            if (const auto* member = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
                // `this.#x = v` installs no PROPERTY: a private element lives
                // in a side table keyed by the class evaluation, and no shape
                // ever carries its name. A shape class that listed it would
                // describe a layout no instance has.
                if (!member->isPrivate &&
                    dynamic_cast<const ast::ThisExpr*>(member->object.get())) {
                    if (std::find(properties.begin(), properties.end(), member->property) ==
                        properties.end()) {
                        properties.push_back(member->property);
                    }
                }
            }
        }
        Walker::visit(n);
    }
};

}  // namespace

// ---- expressions -------------------------------------------------------

Type FlowAnalyzer::expr(const ast::Expr& e) {
    const Type t = exprKind(e);
    if (record_) mod_.result->exprTypes[&e] = t;
    // EVERY round, not only the recording one: the audit is part of the outer
    // fixpoint and has to see the types each round produced. It ignores every
    // expression that is not the right-hand side of a write it recorded, so
    // this is one lookup in a table sized by the program's property writes.
    mod_.fieldAudit.observe(&e, t);
    return t;
}

Type FlowAnalyzer::exprKind(const ast::Expr& e) {
    if (dynamic_cast<const ast::NumberLit*>(&e)) return Type::number();
    // DYNAMIC, and deliberately not `number`: the lattice has no BigInt
    // element, and a BigInt typed as one would licence an f64 fast path to
    // read a heap pointer's bits as a double. Dynamic is the designed
    // fallback and the only answer here that cannot be wrong.
    if (dynamic_cast<const ast::BigIntLit*>(&e)) return Type::dynamic();
    if (dynamic_cast<const ast::StringLit*>(&e)) return Type::string();
    // A regular expression literal is an OBJECT, and inference has no shape
    // class for one: a RegExp carries no shape at all, so every read off it
    // goes through the runtime's own branch.
    if (dynamic_cast<const ast::RegExpLit*>(&e)) return Type::dynamic();
    // A template is a string whatever its substitutions produce, since
    // every one of them goes through ToString. The substitutions are
    // still analysed — they are ordinary expressions and may write
    // bindings.
    if (const auto* t = dynamic_cast<const ast::TemplateLit*>(&e)) {
        for (const auto& sub : t->exprs) expr(*sub);
        return Type::string();
    }
    if (dynamic_cast<const ast::BoolLit*>(&e)) return Type::boolean();
    if (dynamic_cast<const ast::NullLit*>(&e)) return Type::null();
    if (dynamic_cast<const ast::UndefinedLit*>(&e)) return Type::undefined();
    // `this` is the caller's receiver. Inside a class body the DECLARATION
    // names the object kind that receiver is meant to be, and `Scope::thisClass`
    // carries it — see the standing invariant recorded there for why an
    // optimistic answer is safe, and what would stop making it safe.
    if (dynamic_cast<const ast::ThisExpr*>(&e)) {
        // NOT BUILT HERE. The declaration says what the receiver is MEANT to
        // be; `Vector3.prototype.add.call(x)` says what it can actually be, and
        // nothing in this file can see that call. The identity is still worth
        // having — a shape guard checks it, and a wrong guess costs a miss —
        // but a claim about what is INSIDE the object is not guarded by
        // anything, so `this.x` may not be typed `number` on this evidence.
        return scope_.thisClass == kNoShapeClass ? Type::dynamic()
                                                 : Type::objectNotBuiltHere(scope_.thisClass);
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
        const Type t = lookup(id->name);
        noteIdentRefusal(*id, t);
        return t;
    }
    if (const auto* u = dynamic_cast<const ast::Unary*>(&e)) return unary(*u);
    if (const auto* b = dynamic_cast<const ast::Binary*>(&e)) return binary(*b);
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&e)) {
        expr(*t->condition);
        const Env entry = scope_.env;
        const Type a = expr(*t->thenExpr);
        const Env thenEnv = scope_.env;
        scope_.env = entry;
        const Type b = expr(*t->elseExpr);
        scope_.env = joinEnv(thenEnv, scope_.env);
        return join(a, b);
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&e)) {
        const Type base = expr(*m->object);
        lastMember_ = m;
        lastMemberBase_ = base;
        // A read from a value that never arrives produces a value that never
        // arrives. Answering `Dynamic` here instead would be a claim, and the
        // wrong one: `Never` is the bottom of this lattice and only widens, so a
        // base that is `Never` on one round of the call-graph fixpoint and a
        // class on the next would have made this expression's type NARROW
        // between rounds. The interprocedural pass (method_ident.h) reads
        // receiver types to decide which methods it can still speak for, and a
        // decision taken from a narrowing sequence is not a fixpoint — it is
        // whichever round happened to run last.
        if (base.is(TypeKind::Never)) return Type::never();
        // The Math VALUE properties are numbers on the pristine builtin —
        // 21.3.1 lists exactly these eight, all non-writable and
        // non-configurable, though what carries the proof here is the
        // program-wide pristine bit, not the attributes.
        if (!m->optional && isPristineMathBase(*m->object)) {
            if (m->property == "PI" || m->property == "E" || m->property == "LN2" ||
                m->property == "LN10" || m->property == "LOG2E" || m->property == "LOG10E" ||
                m->property == "SQRT1_2" || m->property == "SQRT2") {
                return Type::number();
            }
        }
        // A field of a class the layout analysis modelled: the type joined over
        // every `this.<name> = ...` the class body writes. This is what carries
        // a class identity ACROSS a property read — `object3d.position` is a
        // `Vector3`, so `object3d.position.x` is a proven site too, and the
        // chain of reads three.js is made of stops being one dynamic hop per
        // link. Anything unmodelled joined to `Dynamic` on the way in, so the
        // miss answer is the same one this returned before.
        //
        // Deliberately not applied to `?.`: an optional read of an absent link
        // produces `undefined`, which is not the field's type.
        if (!m->optional && base.is(TypeKind::Object) &&
            base.shapeClass() != kNoShapeClass && !m->isPrivate) {
            const Type field =
                mod_.result->classLayouts.fieldTypeOf(base.shapeClass(), m->property);
            // The IDENTITY travels either way, one rung weaker than the base's:
            // an object read out of a field was not watched being made, so the
            // next link is bounded the same way this one is.
            if (field.is(TypeKind::Object)) {
                return base.identityOnly() ? Type::objectIdentityOnly(field.shapeClass())
                                           : Type::objectNotBuiltHere(field.shapeClass());
            }
            // A PRIMITIVE is a different kind of claim and takes two proofs.
            //
            // The base has to have been watched being made (`builtHere`). Every
            // other rung — `this`, a method parameter, a field read — is a guess
            // that the value is an instance at all, and a guess is checked by a
            // shape guard, which is a check on the OBJECT and not on what the
            // slot holds. A `Vector3`-typed `o.position` that is really the
            // string "hi" reads `undefined` at `.x`, and `undefined` is not a
            // number.
            //
            // And every write in the program has to preserve the type. The
            // harvest that produced `field` read one class body; `v.x = "hi"`
            // three lines below it is a write the harvest never saw, and until
            // the audit existed it reached `mergeParamType`, an f64 block
            // parameter and a hard unbox — `NaN` where the language says `hi`.
            //
            // NUMBER is the only primitive the audit certifies, and the only
            // one worth certifying: it is the one lattice element with unboxed
            // IL ops behind it (`ilTypeOf`). A `string` or `bool` field type
            // would have to carry its own program-wide invariant to be a proof,
            // and would buy a calling convention nothing consumes.
            if (!field.is(TypeKind::Number)) return Type::dynamic();
            // The population and what stopped it, so the report can say what
            // the audit MOVED and not only what it certified. Recorded on the
            // one pass that fills the side table, like every other statistic.
            auto& report = mod_.result->fieldAudit;
            if (record_) ++report.numberFieldReads;
            if (!base.builtHere()) {
                if (record_) ++report.refusedNotBuiltHere;
                return Type::dynamic();
            }
            if (!mod_.result->classLayouts.fieldValueCandidate(base.shapeClass(), m->property)) {
                if (record_) ++report.refusedByClass;
                return Type::dynamic();
            }
            if (!mod_.fieldAudit.numberClean(m->property)) {
                if (record_) ++report.refusedByAudit;
                return Type::dynamic();
            }
            if (record_) mod_.result->provenFieldReads.insert(m);
            return field;
        }
        // Otherwise: the receiver's shape class is proven, never the property's
        // type; that is what the inline-cache check consumes and all it needs.
        return Type::dynamic();
    }
    if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&e)) {
        expr(*ix->object);
        expr(*ix->index);
        return Type::dynamic();
    }
    if (const auto* c = dynamic_cast<const ast::Call*>(&e)) return call(*c);
    if (const auto* n = dynamic_cast<const ast::NewExpr*>(&e)) return newExpr(*n);
    if (dynamic_cast<const ast::NewTargetExpr*>(&e)) return Type::dynamic();
    if (dynamic_cast<const ast::ImportMetaExpr*>(&e)) return Type::dynamic();
    if (const auto* tt = dynamic_cast<const ast::TaggedTemplate*>(&e)) {
        expr(*tt->tag);
        for (const auto& el : tt->templateLit->exprs) expr(*el);
        return Type::dynamic();
    }
    if (const auto* o = dynamic_cast<const ast::ObjectLit*>(&e)) return objectLit(*o);
    if (const auto* a = dynamic_cast<const ast::ArrayLit*>(&e)) {
        for (const auto& el : a->elements) {
            if (el) expr(*el);
        }
        // An array is an object with no property-name identity; there is
        // no shape class to prove about it.
        return Type::object();
    }
    if (const auto* sc = dynamic_cast<const ast::SuperCall*>(&e)) {
        // The parent constructor runs on the current receiver and its
        // result is discarded, so nothing is proven about the value.
        for (const auto& a : sc->args) expr(*a);
        return Type::dynamic();
    }
    if (dynamic_cast<const ast::SuperMember*>(&e)) return Type::dynamic();
    if (const auto* ce = dynamic_cast<const ast::ClassExpr*>(&e)) {
        analyzeClassBody(ce->name, ce->methods);
        return Type::function();
    }
    // The value of a `yield` is the argument of the `next(v)` that resumed the
    // generator, which comes from outside this compilation entirely. Its
    // operand is still analysed: it is ordinary code that runs here.
    if (const auto* y = dynamic_cast<const ast::YieldExpr*>(&e)) {
        expr(*y->argument);
        return Type::dynamic();
    }
    if (const auto* di = dynamic_cast<const ast::DynamicImportExpr*>(&e)) {
        if (di->specifier) expr(*di->specifier);
        return Type::dynamic();
    }
    // A spread contributes its argument's effects and nothing about the
    // container's element types — there is no element type here to prove.
    if (const auto* sp = dynamic_cast<const ast::SpreadElement*>(&e)) {
        expr(*sp->argument);
        return Type::dynamic();
    }
    // Every name a destructuring assignment writes becomes dynamic: the pieces
    // come out of an indexed or keyed read, and this pass tracks no element or
    // property types to say anything narrower. Assigning rather than ignoring
    // is the point — a name proven numeric before must not stay numeric across
    // it.
    if (const auto* da = dynamic_cast<const ast::DestructuringAssign*>(&e)) {
        const Type value = expr(*da->value);
        for (const auto& name : ast::patternBoundNames(*da->pattern)) {
            assign(name, Type::dynamic());
        }
        return value;
    }
    if (const auto* f = dynamic_cast<const ast::FunctionExpr*>(&e)) {
        // 15.3.4: an arrow has no `this` binding of its own and resolves the
        // name in the enclosing scope, so it inherits whatever receiver is in
        // hand. A non-arrow function expression binds its own, which the
        // caller supplies and no declaration here can name.
        analyzeNested(*f, f->name, f->params, f->body, f->span,
                      f->isGenerator || f->isAsync,
                      f->isArrow ? scope_.thisClass : kNoShapeClass);
        return Type::function();
    }
    fail(e.span, "saw an unknown expression node kind");
    return Type::dynamic();
}

Type FlowAnalyzer::unary(const ast::Unary& u) {
    const Type operand = expr(*u.operand);
    const Type result = unaryResult(u.op, operand);
    // The update forms are the one place a USE site sharpens a name — but
    // only to what 13.4.4 actually stores: ToNumeric of the old value, which
    // `unaryResult` already computed, and which is a Number only when the
    // operand can never be a BigInt. Writing `number` unconditionally here
    // was the unsound claim that let `let c = 1n; c++` unbox a BigInt as an
    // f64 wherever a later read consumed the proof.
    if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PreDec ||
        u.op == ast::UnaryOp::PostInc || u.op == ast::UnaryOp::PostDec) {
        if (const auto* id = dynamic_cast<const ast::Ident*>(u.operand.get())) {
            assign(id->name, result);
        }
    }
    return result;
}

Type FlowAnalyzer::binary(const ast::Binary& b) {
    if (b.op == ast::BinaryOp::Assign) {
        const Type rhs = expr(*b.rhs);
        if (const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get())) {
            assign(id->name, rhs);
        } else {
            expr(*b.lhs);
        }
        return rhs;
    }
    if (ast::isCompoundAssignOp(b.op)) {
        const ast::BinaryOp plain = ast::compoundAssignBase(b.op);
        const Type rhs = expr(*b.rhs);
        const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get());
        const Type current = id != nullptr ? lookup(id->name) : expr(*b.lhs);
        const Type result = compoundResult(plain, current, rhs);
        if (id != nullptr) assign(id->name, result);
        return result;
    }

    // `&&` / `||` / `??` are short-circuiting, so the right operand may
    // not run; joining its environment back in would be a claim it did.
    // The value is one operand or the other, hence the type join.
    if (b.op == ast::BinaryOp::LogicalAnd || b.op == ast::BinaryOp::LogicalOr ||
        b.op == ast::BinaryOp::NullishCoalescing) {
        const Type l = expr(*b.lhs);
        const Env entry = scope_.env;
        const Type r = expr(*b.rhs);
        scope_.env = joinEnv(entry, scope_.env);
        return join(l, r);
    }

    const Type l = expr(*b.lhs);
    const Type r = expr(*b.rhs);
    return binaryResult(b.op, l, r);
}

Type FlowAnalyzer::call(const ast::Call& c) {
    const Type calleeType = expr(*c.callee);
    // The receiver of a method call, recovered from the one evaluation of the
    // callee rather than by walking its base a second time. `lastMember_` is the
    // outermost member read that evaluation finished with, so the pointer
    // compare is what confirms this call really is on it.
    const auto* member = dynamic_cast<const ast::MemberAccess*>(c.callee.get());
    const Type receiver = (member != nullptr && lastMember_ == member) ? lastMemberBase_
                                                                       : Type::dynamic();
    std::vector<Type> args;
    args.reserve(c.args.size());
    bool spreadArgs = false;
    for (const auto& a : c.args) {
        if (dynamic_cast<const ast::SpreadElement*>(a.get())) spreadArgs = true;
        args.push_back(expr(*a));
    }

    // `Math.<fn>(...)` on the pristine builtin: every OWN function property
    // of 21.3 returns a Number for ANY arguments (a BigInt argument throws
    // before a value exists, which claims nothing). The list is the own
    // methods only — an INHERITED call like `Math.toString()` reaches
    // Object.prototype and is deliberately absent.
    if (!c.optional && mathCallReturnsNumber(c)) {
        if (record_) mod_.result->pristineMathCalls.insert(&c);
        return Type::number();
    }

    // `recv.m(...)`: the call sites a class METHOD has, enumerated through the
    // receiver's class rather than through the callee's name.
    if (member != nullptr) return methodCall(member->property, receiver, args, spreadArgs);
    // `super.m(...)`. The receiver is this method's `this` — a subclass instance
    // — but the LOOKUP starts at the enclosing class's base, which is the one
    // thing about the dispatch that is decided statically. So the contribution
    // goes to whatever `m` that base would find, and to the overrides below it,
    // exactly as an ordinary call on a base-typed receiver does.
    if (const auto* sup = dynamic_cast<const ast::SuperMember*>(c.callee.get())) {
        const ClassLayout* here = mod_.result->classLayouts.byShapeClass(scope_.thisClass);
        if (here == nullptr || here->superName.empty()) {
            if (mod_.interprocIdent && mod_.methods.isMethodName(sup->property)) {
                mod_.methodPoison.add(sup->property, "a `super` call whose base class is unknown");
            }
            return Type::dynamic();
        }
        const ClassLayout* base = mod_.result->classLayouts.byName(here->superName);
        if (base == nullptr) {
            if (mod_.interprocIdent && mod_.methods.isMethodName(sup->property)) {
                mod_.methodPoison.add(sup->property, "a `super` call whose base class is unknown");
            }
            return Type::dynamic();
        }
        return methodCall(sup->property, Type::object(base->shapeClass), args, spreadArgs);
    }

    const uint32_t index = calleeType.functionIndex();
    if (index == kNoFunctionIndex) return Type::dynamic();

    FunctionInfo& callee = mod_.functions[index];
    if (!callee.directCallable) return Type::dynamic();

    // This site's contribution to the callee's parameters. A missing
    // argument is `undefined`, exactly as the call would deliver it.
    for (size_t i = 0; i < callee.observedParams.size(); ++i) {
        const Type at = i < args.size() ? args[i] : Type::undefined();
        callee.observedParams[i] = join(callee.observedParams[i], at);
    }
    return callee.signature.returnType;
}

const ClassLayout* FlowAnalyzer::receiverClass(Type receiver) const {
    if (!receiver.is(TypeKind::Object) || receiver.shapeClass() == kNoShapeClass) return nullptr;
    const ClassLayout* cl = mod_.result->classLayouts.byShapeClass(receiver.shapeClass());
    return cl == nullptr || cl->name.empty() ? nullptr : cl;
}

// One `recv.name(...)`.
//
// The contribution is the whole mechanism: every method this call can reach
// joins the argument types at this site into its `observedParams`, and
// `widenSignatures` folds those into the signature the method's body is then
// analysed under. Everything else here is the discipline that keeps the join
// from speaking for callers it never saw.
Type FlowAnalyzer::methodCall(const std::string& name, Type receiver,
                              const std::vector<Type>& args, bool spreadArgs) {
    if (!mod_.interprocIdent) return Type::dynamic();
    // No class declares a method by this name, so no parameter of any modelled
    // method is at stake — this is a call on a builtin, on host code, or on a
    // function a property happens to hold.
    if (!mod_.methods.isMethodName(name)) return Type::dynamic();
    // Nothing has reached this receiver yet. The call-graph fixpoint has not
    // finished; a round that read this as "unproven" would be poisoning the name
    // on evidence that does not exist.
    if (receiver.is(TypeKind::Never)) return Type::never();

    const ClassLayout* cls = receiverClass(receiver);
    if (cls == nullptr) {
        mod_.methodPoison.add(name, "called on a receiver whose class is not proven");
        return Type::dynamic();
    }
    if (spreadArgs) {
        mod_.methodPoison.add(name, "an argument list is spread at a call site");
        return Type::dynamic();
    }
    std::vector<uint32_t> targets;
    mod_.methods.reachableFrom(cls->name, name, targets);
    // The receiver's class declares nothing by this name and neither does
    // anything it extends: the property is inherited from outside the modelled
    // classes, or absent. No modelled method is reached, so none is poisoned.
    if (targets.empty()) return Type::dynamic();

    Type ret = Type::never();
    bool everyTargetSpeaks = true;
    for (const uint32_t index : targets) {
        MethodInfo& target = mod_.methods.methods()[index];
        // A missing argument is `undefined`, exactly as the call delivers it.
        for (size_t i = 0; i < target.observedParams.size(); ++i) {
            const Type at = i < args.size() ? args[i] : Type::undefined();
            target.observedParams[i] = join(target.observedParams[i], at);
        }
        if (mod_.methodPoison.poisons(target.methodName)) {
            everyTargetSpeaks = false;
            continue;
        }
        ret = join(ret, target.signature.returnType);
    }
    // The value: what every method this dispatch can reach returns. An identity
    // out of here is as good as the dispatch that chose it, which is a guess —
    // so it travels as one. `return this` is the case that pays: three.js
    // methods chain, and `v.copy(u).add(w)` is two sites, not one.
    if (!everyTargetSpeaks || ret.is(TypeKind::Never)) return Type::dynamic();
    if (ret.is(TypeKind::Object) && ret.shapeClass() != kNoShapeClass) {
        return Type::objectIdentityOnly(ret.shapeClass());
    }
    // Only an identity is worth carrying out of an optimistic dispatch. A
    // primitive return type would be a claim about a VALUE, which is what
    // `Type::objectIdentityOnly` exists to keep this mechanism out of.
    return Type::dynamic();
}

void FlowAnalyzer::noteIdentRefusal(const ast::Ident& id, Type resolved) {
    if (!record_ || !mod_.interprocIdent) return;
    if (!resolved.is(TypeKind::Dynamic)) return;
    if (scope_.methodIndex == kNoMethod) return;
    const MethodInfo& self = mod_.methods.methods()[scope_.methodIndex];
    const auto& names = self.fn->params;
    bool isParam = false;
    for (const auto& p : names) {
        if (p.name == id.name) {
            isParam = true;
            break;
        }
    }
    if (!isParam) return;
    std::string reason;
    if (!self.plainParams) {
        reason = "the method's parameter list is not plain";
    } else if (mod_.methodPoison.poisons(self.methodName)) {
        reason = mod_.methodPoison.reasonFor(self.methodName);
    } else if (self.unreached) {
        reason = "no call site this compilation saw reaches the method";
    } else {
        reason = "the call sites disagree about the argument's class";
    }
    mod_.result->identRefusals.emplace(&id, std::move(reason));
}

Type FlowAnalyzer::newExpr(const ast::NewExpr& n) {
    // A bare NAME is the only callee whose constructor identity is knowable
    // here, and identity is what a shape class is: `new Foo()` names the
    // function whose `this.x = ...` assignments describe the layout, where
    // `new obj.Ctor()` names a value the analysis cannot follow back to one.
    // The unproven site gets `kNoShapeClass`, which is the same answer an
    // unknown name already produced — its property sites stay polymorphic
    // rather than guessing a layout.
    const auto* ident = dynamic_cast<const ast::Ident*>(n.callee.get());
    // The callee is evaluated before the arguments (ECMA-262 13.3.5.1), so
    // its effects are recorded first. A bare name is deliberately not walked:
    // reading it is not what `new` does with it, and `constructorShape` below
    // is the fact this site contributes about that name.
    if (ident == nullptr) expr(*n.callee);
    for (const auto& a : n.args) expr(*a);
    // `new Float64Array(...)` on the UNSHADOWED name is the builtin: assigning
    // to a builtin global is a compile error, so the only way the name can
    // mean anything else is a program binding — which `resolvesToUserBinding`
    // sees, module scope and imports included. The argument forms all produce
    // a view with this element kind, so the arguments do not matter here.
    if (ident != nullptr && !resolvesToUserBinding(ident->name)) {
        if (ident->name == "Float64Array") return Type::typedArray(TypedArrayElem::Float64);
        if (ident->name == "Float32Array") return Type::typedArray(TypedArrayElem::Float32);
    }
    const ShapeClassId cls = ident != nullptr ? constructorShape(ident->name) : kNoShapeClass;
    if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
    return Type::object(cls);
}

ShapeClassId FlowAnalyzer::constructorShape(const std::string& name) {
    // A `class` first: it is the form three.js is written in, and its identity
    // is already interned by the layout analysis, which knows things this
    // walker does not — the `extends` prefix, and field declarations.
    if (const ClassLayout* cl = mod_.result->classLayouts.byName(name)) {
        // A class name that is also a module function name is impossible: both
        // are module-scope bindings and the parser would have rejected the
        // redeclaration. Checking the class table first is therefore an
        // ordering, not a precedence rule.
        return cl->shapeClass;
    }
    const auto found = mod_.indexByName.find(name);
    if (found == mod_.indexByName.end()) return kNoShapeClass;
    const uint32_t index = found->second;
    if (const auto it = mod_.ctorShapes.find(index); it != mod_.ctorShapes.end()) {
        return it->second;
    }
    ThisPropertyWalker walker;
    walker.walkList(mod_.functions[index].decl->body);
    const ShapeClassId cls = mod_.result->shapes.intern(name, std::move(walker.properties));
    mod_.ctorShapes.emplace(index, cls);
    return cls;
}

Type FlowAnalyzer::objectLit(const ast::ObjectLit& o) {
    std::vector<std::string> props;
    bool computedKey = false;
    for (const auto& p : o.props) {
        // Key then value, left to right, which is the order the language
        // specifies and therefore the order the effects are recorded in.
        if (p.keyExpr) {
            computedKey = true;
            expr(*p.keyExpr);
        }
        expr(*p.value);
        if (p.keyExpr) continue;
        // A duplicate key overwrites; it does not transition again.
        if (std::find(props.begin(), props.end(), p.key) == props.end()) {
            props.push_back(p.key);
        }
    }
    // A computed key names its property only at run time, so this literal's
    // own-property set is not known here. A shape class interned over the
    // WRITTEN keys alone would be a claim about a layout the runtime never
    // builds, and the inline caches rest on that claim being true — so a
    // literal with any computed key is simply `dynamic`, and its sites stay
    // polymorphic.
    if (computedKey) return Type::dynamic();
    // Empty constructor name: a plain literal's prototype is the one root shape
    // every `{}` shares.
    const ShapeClassId cls = mod_.result->shapes.intern(std::string(), std::move(props));
    if (record_) mod_.result->siteShapes[&o] = cls;
    return Type::object(cls);
}

Type FlowAnalyzer::analyzeNested(const ast::Node& site, const std::string& declaredName,
                   const std::vector<ast::Param>& params,
                   const std::vector<ast::StmtPtr>& body, Span span, bool isGenerator,
                   ShapeClassId thisClass, uint32_t methodIndex) {
    std::string name = declaredName;
    if (name.empty()) name = "<anon" + std::to_string(anonCounter_++) + ">";
    std::vector<const ast::Stmt*> borrowed;
    borrowed.reserve(body.size());
    for (const auto& s : body) borrowed.push_back(s.get());

    // A closure is never a direct-call target, so its parameters keep the
    // uniform dynamic convention. A class METHOD is the exception this chunk
    // added: its callers are enumerable through the receiver's class, so it has
    // a signature of its own (method_ident.h). Nothing about the CALLING
    // CONVENTION changes either way — a method is still invoked dynamically;
    // what the signature buys is what the body may believe about its arguments.
    std::vector<Type> paramTypes(params.size(), Type::dynamic());
    if (methodIndex != kNoMethod) {
        const MethodInfo& self = mod_.methods.methods()[methodIndex];
        if (self.plainParams && !mod_.methodPoison.poisons(self.methodName)) {
            for (size_t i = 0; i < paramTypes.size() && i < self.signature.params.size(); ++i) {
                const Type proven = self.signature.params[i];
                // An IDENTITY only. A parameter every seen caller passes a
                // number to is not thereby a number: the dispatch that chose
                // this method is a guess, and `Number` licenses unboxed f64,
                // which no guess may license. An object identity licenses the
                // guarded property-site form and nothing else, which is exactly
                // what a guess can afford.
                if (proven.is(TypeKind::Object) && proven.shapeClass() != kNoShapeClass) {
                    paramTypes[i] = Type::objectIdentityOnly(proven.shapeClass());
                } else if (proven.is(TypeKind::Never)) {
                    // Still waiting on the fixpoint. `Never` is what says so.
                    paramTypes[i] = proven;
                }
            }
        }
    }
    const FunctionOutcome outcome =
        analyzeFunction(mod_, &scope_, qualifiedName_ + "::" + name, kNoFunctionIndex, &site,
                        /*directCallable=*/false, params, paramTypes, borrowed, span, record_,
                        isGenerator, thisClass, methodIndex);
    return outcome.returnType;
}

// A class body, walked with `this` bound to the class the declaration names.
//
// One routine for `ClassDecl` and `ClassExpr` because the two nodes differ in
// nothing this pass reads. The receiver goes to instance members only: a static
// method's `this` is the CONSTRUCTOR, whose own layout is a different object's,
// and this analysis models instances.
void FlowAnalyzer::analyzeClassBody(const std::string& className,
                                    const std::vector<ast::ClassMethod>& methods) {
    ShapeClassId owner = kNoShapeClass;
    if (!className.empty()) {
        if (const ClassLayout* cl = mod_.result->classLayouts.byName(className)) {
            owner = cl->shapeClass;
        }
    }
    for (const auto& m : methods) {
        if (m.keyExpr) expr(*m.keyExpr);
        const ShapeClassId receiver = m.isStatic ? kNoShapeClass : owner;
        if (m.fn) {
            const uint32_t index =
                mod_.interprocIdent ? mod_.methods.indexOfNode(m.fn.get()) : kNoMethod;
            const Type returned =
                analyzeNested(*m.fn, m.fn->name, m.fn->params, m.fn->body, m.fn->span,
                              m.fn->isGenerator || m.fn->isAsync, receiver, index);
            if (index != kNoMethod) {
                MethodInfo& self = mod_.methods.methods()[index];
                self.observedReturn = join(self.observedReturn, returned);
            }
        } else if (m.init) {
            // A field initializer is evaluated with `this` already bound to the
            // instance being constructed (15.7.10), so it sees the receiver too
            // — but it runs in THIS walker's scope, not a nested one, so the
            // binding has to be installed and taken back around it.
            const ShapeClassId saved = scope_.thisClass;
            scope_.thisClass = receiver;
            expr(*m.init);
            scope_.thisClass = saved;
        }
    }
}

}  // namespace bronze::types
