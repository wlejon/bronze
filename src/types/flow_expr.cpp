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

bool isNonClassExpr(const ast::Expr* e) {
    if (e == nullptr) return false;
    if (const auto* id = dynamic_cast<const ast::Ident*>(e)) {
        static const char* kNonClassIdents[] = {
            "Math", "Object", "Array", "Number", "String", "Boolean", "Symbol",
            "Reflect", "JSON", "console", "document", "window", "performance",
            "navigator", "self", "gl", "array", "dst", "src", "elements", "te",
            "target", "out", "data", "buffer", "list", "stack", "queue", "nodes",
            "items", "cache", "bindings", "actions", "tracks", "curves", "points",
            "faces", "bones", "lights", "cameras", "materials", "geometries",
            "textures", "objects", "children", "parents", "coords", "weights",
            "times", "samples", "table", "map", "dict"
        };
        for (const char* nid : kNonClassIdents) {
            if (id->name == nid) return true;
        }
    }
    return dynamic_cast<const ast::ArrayLit*>(e) || dynamic_cast<const ast::ObjectLit*>(e) ||
           dynamic_cast<const ast::NumberLit*>(e) || dynamic_cast<const ast::StringLit*>(e) ||
           dynamic_cast<const ast::BoolLit*>(e) || dynamic_cast<const ast::RegExpLit*>(e);
}

// How one class-layout refusal of a number field read is named in the report:
// the receiver, the property, and which condition failed. The receiver is the
// declared class name when there is one and the shape's own rendering when
// there is not, because a refusal on an object literal is a different piece of
// work from one on a class and the report has to be able to say which.
std::string classRefusalKey(const InferenceResult& result, ShapeClassId cls,
                            const std::string& field) {
    const ClassLayout* cl = result.classLayouts.byShapeClass(cls);
    const std::string receiver =
        cl != nullptr ? cl->name : result.shapes.describe(cls);
    return receiver + "." + field + ": " +
           result.classLayouts.fieldValueRefusal(cls, field);
}

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
        if (!m->optional && isPristineMathBase(*m->object) &&
            (m->property == "PI" || m->property == "E" || m->property == "LN2" ||
             m->property == "LN10" || m->property == "LOG2E" || m->property == "LOG10E" ||
             m->property == "SQRT1_2" || m->property == "SQRT2")) {
            return Type::number();
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
            Type field =
                mod_.result->classLayouts.fieldTypeOf(base.shapeClass(), m->property);
            // The harvest has not decided this field yet: its only writes are
            // constructor parameters the call-graph fixpoint is still joining
            // (types/ctor_ident.h). `Never` is how "undecided" is spelled here,
            // and answering it is what keeps this expression's type MONOTONE
            // over the rounds. `Dynamic` would be the top of the lattice on a
            // round whose answer is `number` on the next one — and the write
            // audit's refusals are STICKY, so one such round refuses the name
            // for the rest of the compilation on evidence that does not exist.
            if (field.is(TypeKind::Never)) return Type::never();
            // A PINNED field (`--pins`, types/pins.h): the manifest DECLARES
            // what this slot holds, and the read spends that declaration where
            // the three proofs below would stand it down. Nothing checks it
            // here — enforcement belongs on the write paths, and until that
            // machinery exists the manifest IS the enforcement. Which is
            // exactly why it is a named per-(class, field) list: the blanket
            // form pinned `Object3D.children` too, and an object read out of a
            // pinned array is a pointer's bits read as a double.
            if (const PinKind* pin = pinnedField(base.shapeClass(), m->property)) {
                if (*pin == PinKind::NumericElements) return Type::arrayPinnedF64();
                // The NULLISH-WIDENED pin declares a Number-or-nullish slot,
                // which is not a lattice element and must not be forced into
                // one. The site is recorded and the TYPE stays `Dynamic`, so
                // `typeof this.limit` and `this.limit === null` keep the
                // dynamic answers that are the correct ones — only lowering's
                // coercing positions read the set, and only they may.
                if (*pin == PinKind::NumberOrNullish) {
                    if (record_) mod_.result->nullishNumberFieldReads.insert(m);
                    return Type::dynamic();
                }
                if (record_) {
                    ++mod_.result->fieldAudit.numberFieldReads;
                    mod_.result->provenFieldReads.insert(m);
                }
                return Type::number();
            }
            // The IDENTITY travels either way, one rung weaker than the base's:
            // an object read out of a field was not watched being made, so the
            // next link is bounded the same way this one is.
            if (field.is(TypeKind::Object)) {
                return base.identityOnly() ? Type::objectIdentityOnly(field.shapeClass())
                                           : Type::objectNotBuiltHere(field.shapeClass());
            }
            if (field.is(TypeKind::Dynamic) && mod_.fieldAudit.numberClean(m->property)) {
                field = Type::number();
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
            // Probe: an Array/TypedArray field keeps its kind across the
            // read (it is an identity-grade claim here, spent only on the
            // element form), and the three primitive proofs below are
            // skipped. See flow.h `unsoundPins`.
            if (mod_.unsoundPins &&
                (field.is(TypeKind::Array) || field.is(TypeKind::TypedArray))) {
                return field;
            }
            if (!field.is(TypeKind::Number)) return Type::dynamic();
            if (record_) ++mod_.result->fieldAudit.numberFieldReads;
            if (mod_.unsoundPins) {
                if (record_) mod_.result->provenFieldReads.insert(m);
                return field;
            }
            if (!mod_.methodParamTypes && !base.builtHere()) {
                if (record_) ++mod_.result->fieldAudit.refusedNotBuiltHere;
                return Type::dynamic();
            }
            if (!mod_.result->classLayouts.fieldValueCandidate(base.shapeClass(), m->property)) {
                if (record_) {
                    ++mod_.result->fieldAudit.refusedByClass;
                    ++mod_.result->fieldAudit.classRefusedSites[classRefusalKey(
                        *mod_.result, base.shapeClass(), m->property)];
                }
                return Type::dynamic();
            }
            if (!mod_.fieldAudit.numberClean(m->property)) {
                if (record_) ++mod_.result->fieldAudit.refusedByAudit;
                return Type::dynamic();
            }
            if (record_) {
                mod_.result->provenFieldReads.insert(m);
            }
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
        return Type::array();
    }
    if (const auto* sc = dynamic_cast<const ast::SuperCall*>(&e)) {
        // The parent constructor runs on the current receiver and its
        // result is discarded, so nothing is proven about the value.
        std::vector<Type> args;
        args.reserve(sc->args.size());
        bool spreadArgs = false;
        for (const auto& a : sc->args) {
            if (dynamic_cast<const ast::SpreadElement*>(a.get())) spreadArgs = true;
            args.push_back(expr(*a));
        }
        // It is also a CALL SITE of the base's constructor, and in three.js it
        // is the commonest one there is: every class below `Object3D` reaches
        // its fields through one. Which constructor that is, is decided
        // statically — the enclosing class's base, and then whatever that base
        // forwards to when it declares none.
        // The implicit `constructor(...args) { super(...args) }` is a link and
        // not a call site: what reaches the base is what reached the subclass,
        // and `CtorTable::targetOf` already walks straight through it. Reading
        // this spread as evidence would poison the base of every bare
        // `class X extends Y {}` in the program.
        const bool forwarding = scope_.ctorIndex != kNoCtor &&
                                mod_.ctors.ctors()[scope_.ctorIndex].isForwarder;
        if (mod_.ctorParamTypes && !forwarding) {
            const ClassLayout* here = mod_.result->classLayouts.byShapeClass(scope_.thisClass);
            // A base this pass cannot name — an anonymous enclosing class, or
            // `extends (expr)` — needs no poison of its own: the only road from
            // there to one of the program's classes is a read of that class's
            // binding, and such a read is an escape the scan already saw.
            if (here != nullptr && !here->superName.empty()) {
                constructSite(here->superName, args, spreadArgs);
            }
        }
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

    // A pinned RETURN, spent HERE and not only on the callee's own signature.
    // Lowering has always turned `return <owner>: number` into an f64 result on
    // the callee's typed entry, and that is half a promise: the value arrives
    // in a register and the caller then boxes it, because the analysis that
    // types the caller's arithmetic — and, decisively, a loop-carried binding
    // at its merge — never heard the claim. `hits = hits + useProgram(i)` stayed
    // a `bronze_dynamic_add` per iteration over an f64 the callee had already
    // computed. A pin is a promise the invocation makes to the whole
    // compilation, so it is answered wherever the compilation asks.
    //
    // A closure is exactly the case that needs it: its callers are reached
    // through a function value, so it has no `functionIndex` and no signature
    // below can speak for it. `?.()` is excluded — the call may not happen and
    // the result is then `undefined`, which is not a Number.
    //
    // NOTED, not returned. A pin about the RESULT says nothing about the
    // ARGUMENTS, and answering here used to skip the contribution loop at the
    // bottom of this function — so `return run: number` on a direct call
    // `run(iters)` silently un-typed `run`'s own parameter, and the census
    // emitting that entry cost 1.8 ns/call on mat4_kernel (stage C1). A pin adds
    // a promise; it must never subtract a proof.
    bool pinnedReturn = false;
    if (!c.optional && mod_.pins != nullptr) {
        const auto* ident = dynamic_cast<const ast::Ident*>(c.callee.get());
        if (ident != nullptr && mod_.pins->returnPinned(ident->name)) pinnedReturn = true;
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
                mod_.methodPoison.addDeclarations(mod_.methods, sup->property,
                                                 "a `super` call whose base class is unknown");
            }
            return Type::dynamic();
        }
        const ClassLayout* base = mod_.result->classLayouts.byName(here->superName);
        if (base == nullptr) {
            if (mod_.interprocIdent && mod_.methods.isMethodName(sup->property)) {
                mod_.methodPoison.addDeclarations(mod_.methods, sup->property,
                                                 "a `super` call whose base class is unknown");
            }
            return Type::dynamic();
        }
        return methodCall(sup->property, Type::object(base->shapeClass), args, spreadArgs);
    }

    const uint32_t index = calleeType.functionIndex();
    if (index == kNoFunctionIndex) return pinnedReturn ? Type::number() : Type::dynamic();

    FunctionInfo& callee = mod_.functions[index];
    if (!callee.directCallable) return pinnedReturn ? Type::number() : Type::dynamic();

    // This site's contribution to the callee's parameters. A missing
    // argument is `undefined`, exactly as the call would deliver it.
    for (size_t i = 0; i < callee.observedParams.size(); ++i) {
        const Type at = i < args.size() ? args[i] : Type::undefined();
        callee.observedParams[i] = join(callee.observedParams[i], at);
    }
    // The pin wins over the join for the RESULT — the invocation's promise is
    // the stronger statement, and it is the one the callee's typed entry was
    // built from (`applySignaturePins`). The contribution above happened either
    // way.
    if (pinnedReturn) return Type::number();
    return callee.signature.returnType;
}

const ClassLayout* FlowAnalyzer::receiverClass(Type receiver) const {
    if (!receiver.is(TypeKind::Object) || receiver.shapeClass() == kNoShapeClass) return nullptr;
    const ClassLayout* cl = mod_.result->classLayouts.byShapeClass(receiver.shapeClass());
    return cl == nullptr || cl->name.empty() ? nullptr : cl;
}

const PinKind* FlowAnalyzer::pinnedField(ShapeClassId cls, const std::string& field) const {
    if (mod_.pins == nullptr || cls == kNoShapeClass) return nullptr;
    const ClassLayout* layout = mod_.result->classLayouts.byShapeClass(cls);
    // A shape class with no class layout is still nameable: `new F()` interns
    // the constructor's name, which is what a manifest entry spells.
    std::string name =
        layout != nullptr ? layout->name : mod_.result->shapes.at(cls).constructorName;
    if (name.empty()) return nullptr;
    // Bounded rather than trusting `extends` to be acyclic: a cyclic chain is a
    // TypeError at run time and must not be an infinite loop here.
    for (uint32_t hop = 0; hop < kMaxExtendsHops; ++hop) {
        if (const PinKind* pin = mod_.pins->lookup(name, field)) return pin;
        if (layout == nullptr || layout->superName.empty()) return nullptr;
        layout = mod_.result->classLayouts.byName(layout->superName);
        if (layout == nullptr) return nullptr;
        name = layout->name;
    }
    return nullptr;
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

    if (receiver.is(TypeKind::TypedArray) || receiver.is(TypeKind::Number) ||
        receiver.is(TypeKind::String) || receiver.is(TypeKind::Bool) ||
        receiver.is(TypeKind::Null) || receiver.is(TypeKind::Undefined) ||
        (receiver.is(TypeKind::Object) &&
         (receiver.shapeClass() == kNoShapeClass ||
          mod_.result->classLayouts.byShapeClass(receiver.shapeClass()) == nullptr))) {
        return Type::dynamic();
    }
    if (lastMember_ != nullptr && lastMemberBase_ == receiver &&
        isNonClassExpr(lastMember_->object.get())) {
        return Type::dynamic();
    }

    const ClassLayout* cls = receiverClass(receiver);
    if (cls == nullptr) {
        const auto* decls = mod_.methods.declarationsOf(name);
        if (decls == nullptr || decls->empty()) return Type::dynamic();

        if (spreadArgs) {
            for (const uint32_t index : *decls) {
                mod_.methodPoison.add(index, "an argument list is spread at a call site");
            }
            return Type::dynamic();
        }

        auto contributeArgs = [&](MethodInfo& target) {
            for (size_t i = 0; i < target.observedParams.size(); ++i) {
                if (i < args.size()) {
                    if (target.hasDefault.size() > i && target.hasDefault[i] &&
                        args[i].is(TypeKind::Undefined)) {
                        continue;
                    }
                    // Under pin optimism (flow.h `pinOptimism`): a Dynamic
                    // argument does not poison the join — the optimistic
                    // stand-in for what an offline profile would report as the
                    // site's actual class. The commonest source is an UNCALLED
                    // forwarder (three.js `multiply(m) { return
                    // this.multiplyMatrices(this, m) }`) whose own dynamic
                    // parameter otherwise reaches every hot method, and
                    // `Matrix4.multiplyMatrices` is exactly the method it costs.
                    //
                    // Deliberately NOT per-field, unlike the read path above:
                    // a census profile is what should decide it. What the flag
                    // buys is bounded at the fold (`widenMethods`) instead —
                    // see there for why skipping the contribution outright is a
                    // miscompile and what is done about it.
                    if (mod_.pinOptimism() && args[i].is(TypeKind::Dynamic)) {
                        target.sawSkippedDynamicArg[i] = true;
                        continue;
                    }
                    target.observedParams[i] = join(target.observedParams[i], args[i]);
                } else if (target.hasDefault.size() > i && !target.hasDefault[i]) {
                    target.observedParams[i] = join(target.observedParams[i], Type::undefined());
                }
            }
        };

        for (const uint32_t index : *decls) {
            MethodInfo& target = mod_.methods.methods()[index];
            if (args.size() < target.observedParams.size()) {
                bool hasDefaults = true;
                for (size_t i = args.size(); i < target.observedParams.size(); ++i) {
                    if (target.hasDefault.size() <= i || !target.hasDefault[i]) {
                        hasDefaults = false;
                        break;
                    }
                }
                if (!hasDefaults) continue;
            }
            contributeArgs(target);
        }
        if (record_) ++mod_.unboundedMethodCalls;
        return Type::dynamic();
    }
    std::vector<uint32_t> targets;
    mod_.methods.reachableFrom(cls->name, name, targets);
    if (targets.empty()) return Type::dynamic();

    if (spreadArgs) {
        for (const uint32_t index : targets) {
            mod_.methodPoison.add(index, "an argument list is spread at a call site");
        }
        return Type::dynamic();
    }

    auto contributeArgs = [&](MethodInfo& target) {
        for (size_t i = 0; i < target.observedParams.size(); ++i) {
            if (i < args.size()) {
                if (target.hasDefault.size() > i && target.hasDefault[i] &&
                    args[i].is(TypeKind::Undefined)) {
                    continue;
                }
                // As in the unbounded-receiver path above — under pin optimism
                // a Dynamic argument does not poison the join, and the
                // parameter is marked so the fold can charge for it.
                if (mod_.pinOptimism() && args[i].is(TypeKind::Dynamic)) {
                    target.sawSkippedDynamicArg[i] = true;
                    continue;
                }
                target.observedParams[i] = join(target.observedParams[i], args[i]);
            } else if (target.hasDefault.size() > i && !target.hasDefault[i]) {
                target.observedParams[i] = join(target.observedParams[i], Type::undefined());
            }
        }
    };

    Type ret = Type::never();
    bool everyTargetSpeaks = true;
    for (const uint32_t index : targets) {
        MethodInfo& target = mod_.methods.methods()[index];
        contributeArgs(target);
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
    if (!everyTargetSpeaks || ret.is(TypeKind::Never)) return Type::dynamic();
    if (ret.is(TypeKind::Object) && ret.shapeClass() != kNoShapeClass) {
        return Type::objectIdentityOnly(ret.shapeClass());
    }
    // Only an identity is worth carrying out of an optimistic dispatch. A
    // primitive return type would be a claim about a VALUE, which is what
    // `Type::objectIdentityOnly` exists to keep this mechanism out of.
    return Type::dynamic();
}

// One `new <a class name>(...)`, and every `super(...)` that reaches one.
//
// The site's arguments join into the parameters of whatever constructor the
// name positionally reaches — the class's own, or, when it declares none, the
// one its base reaches, because the implicit constructor forwards everything.
void FlowAnalyzer::constructSite(const std::string& className, const std::vector<Type>& args,
                                 bool spreadArgs) {
    if (!mod_.ctorParamTypes) return;
    const uint32_t target = mod_.ctors.targetOf(className);
    if (target != kNoCtor) contributeCtorArgs(target, args, spreadArgs);
}

// A `new` whose callee is a VALUE: `new this.constructor()`, `new Curves[t]()`,
// `new Ctor(x)` through a parameter.
//
// Which constructors such a site can reach is the whole question this chunk had
// to get right, and the answer is not "all of them". A constructor value gets
// into circulation two ways. The first is a read of the class binding, and that
// read is an escape the scan already poisoned the class for — so those classes
// have nothing left to give up. The second is a read off an OBJECT
// (`o.constructor`, `Object.getPrototypeOf(o)`, `new.target`, a computed read
// that could name `constructor`), and a program that does none of those cannot
// reach an unpoisoned class here at all.
//
// `new <recv>.constructor(...)` is the one form worth being precise about,
// because it is how three.js's `clone()` is written on half its classes: the
// classes it reaches are the ones `recv` can be, which is its class and
// everything that extends it.
void FlowAnalyzer::constructUnbounded(const ast::Expr& callee, Type calleeBase,
                                      const std::vector<Type>& args, bool spreadArgs) {
    if (!mod_.ctorParamTypes) return;
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&callee)) {
        if (m->property == "constructor" && !m->optional) {
            if (const ClassLayout* cls = receiverClass(calleeBase)) {
                std::vector<uint32_t> targets;
                mod_.ctors.subtreeOf(cls->name, targets);
                for (const uint32_t target : targets) contributeCtorArgs(target, args, spreadArgs);
                if (record_) ++mod_.unnamedNewSubtree;
                return;
            }
        }
    }
    if (!mod_.ctorEscapes.valueEscapes) {
        if (record_) ++mod_.unnamedNewIgnored;
        return;
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&callee)) {
        // A top-level `function` used as a constructor — which is how three.js
        // writes its whole WebGL back end, `new WebGLTextures(...)` and thirty
        // more. The name resolves to a module function and to nothing else, so
        // the object it builds is that function's `this`, and no CLASS
        // constructor is reached at all.
        if (lookup(id->name).functionIndex() != kNoFunctionIndex) {
            if (record_) ++mod_.unnamedNewIgnored;
            return;
        }
        // A name the program does not bind is a GLOBAL: `new Error(msg)`,
        // `new Map()`, `new Float64Array(n)`. It can hold one of the program's
        // own classes only if the program writes globals, which is a fact about
        // the text and not about this site.
        if (!resolvesToUserBinding(id->name) && !mod_.ctorEscapes.freeGlobalWrite) {
            if (record_) ++mod_.unnamedNewIgnored;
            return;
        }
    }
    if (spreadArgs) {
        mod_.ctorPoison.addAll("an argument list is spread at a `new` whose callee is a value");
        return;
    }
    if (record_) ++mod_.unnamedNewAll;
    for (uint32_t i = 0; i < mod_.ctors.ctors().size(); ++i) {
        contributeCtorArgs(i, args, /*spreadArgs=*/false);
    }
}

void FlowAnalyzer::contributeCtorArgs(uint32_t ctorIndex, const std::vector<Type>& args,
                                      bool spreadArgs) {
    CtorInfo& info = mod_.ctors.ctors()[ctorIndex];
    if (spreadArgs) {
        mod_.ctorPoison.add(info.className, "an argument list is spread at a construction site");
        return;
    }
    for (size_t i = 0; i < info.observedParams.size(); ++i) {
        if (i < args.size()) {
            // `undefined` at a defaulted position runs the DEFAULT, so the
            // value bound here is not the argument and the argument is not
            // evidence. The default's own type is joined in by the walk that
            // evaluates it (`runParamDefaults`).
            if (info.hasDefault[i] && args[i].is(TypeKind::Undefined)) continue;
            info.observedParams[i] = join(info.observedParams[i], args[i]);
            continue;
        }
        // A missing argument at a position with no default binds `undefined`,
        // exactly as the construction delivers it.
        if (!info.hasDefault[i]) {
            info.observedParams[i] = join(info.observedParams[i], Type::undefined());
        }
    }
}

// What a constructor's body may believe about its own parameters.
//
// The whole proven type, unlike a method's, which is cut down to an identity
// because the dispatch that chose it is a guess. There is no dispatch here:
// `new C(...)` names C, `super(...)` names the base statically, and a class
// whose binding never left `new` position has no other callers. So `Number`
// travels, and an unboxed f64 with it — which is the point of the chunk.
std::vector<Type> FlowAnalyzer::ctorParamTypes(uint32_t ctorIndex, size_t count) const {
    std::vector<Type> out(count, Type::dynamic());
    if (!mod_.ctorParamTypes || ctorIndex == kNoCtor) return out;
    const CtorInfo& self = mod_.ctors.ctors()[ctorIndex];
    if (!self.plainParams || mod_.ctorPoison.poisons(self.className)) return out;
    for (size_t i = 0; i < count && i < self.signature.params.size(); ++i) {
        out[i] = self.signature.params[i];
    }
    return out;
}

// Why one identifier read came back `Dynamic`.
//
// Diagnostics only; nothing types anything from this. It exists because
// "receiver is dynamic: identifier" was the single largest row in the report
// and named a SYNTAX rather than a mechanism — a method parameter the
// interprocedural join could not speak for, a closure parameter no join covers
// at all, and a read of a host global are three unrelated problems wearing one
// label, and a chunk that cannot tell them apart cannot size any of them.
//
// The order below is the order a fix would attack them in, and each branch is
// the mechanism that would have to change.
void FlowAnalyzer::noteIdentRefusal(const ast::Ident& id, Type resolved) {
    if (!record_) return;
    if (!resolved.is(TypeKind::Dynamic)) return;

    // A class method's parameter: the interprocedural pass knows its own reason.
    if (mod_.interprocIdent && scope_.methodIndex != kNoMethod) {
        const MethodInfo& self = mod_.methods.methods()[scope_.methodIndex];
        bool isParam = false;
        for (const auto& p : self.fn->params) {
            if (p.name == id.name) {
                isParam = true;
                break;
            }
        }
        if (isParam) {
            std::string reason;
            if (!self.plainParams) {
                reason = "the method's parameter list is not plain";
            } else if (mod_.methodPoison.poisons(scope_.methodIndex)) {
                reason = mod_.methodPoison.reasonFor(scope_.methodIndex);
            } else if (self.unreached) {
                reason = "no call site this compilation saw reaches the method";
            } else {
                reason = "the call sites disagree about the argument's class";
            }
            mod_.result->identRefusals.emplace(
                &id, "receiver is dynamic: parameter (" + reason + ")");
            return;
        }
    }

    for (const auto& p : facts_.paramNames) {
        if (p == id.name) {
            mod_.result->identRefusals.emplace(&id, "receiver is dynamic: function-value parameter");
            return;
        }
    }

    // A binding of this body, or of a function enclosing it: the flow pass DID
    // resolve the name and what it holds is genuinely unproven — a value out of
    // a dynamic read, or a join of two kinds.
    if (scope_.env.count(id.name) != 0 || scope_.cells.count(id.name) != 0) {
        mod_.result->identRefusals.emplace(&id, "receiver is dynamic: local binding");
        return;
    }
    for (const Scope* p = scope_.parent; p != nullptr; p = p->parent) {
        if (p->cells.count(id.name) != 0) {
            mod_.result->identRefusals.emplace(&id, "receiver is dynamic: captured binding");
            return;
        }
    }

    // A name the MODULE binds, read from a body whose scope chain does not
    // reach the module top level. This is the road value flow opens: a module
    // `const` is one binding for the whole program, so what it holds can be
    // joined program-wide even though no scope chain connects the two.
    if (mod_.moduleScopeNames.count(id.name) != 0) {
        mod_.result->identRefusals.emplace(&id, "receiver is dynamic: module binding");
        return;
    }

    // Nothing in the program declares it: a builtin, or a host global.
    mod_.result->identRefusals.emplace(&id, "receiver is dynamic: global or host name");
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
    //
    // The RECEIVER of a member callee is captured here rather than after the
    // arguments, because walking them overwrites `lastMember_`. It is what
    // bounds `new x.constructor()` to the classes `x` can be.
    Type calleeBase = Type::dynamic();
    if (ident == nullptr) {
        expr(*n.callee);
        const auto* member = dynamic_cast<const ast::MemberAccess*>(n.callee.get());
        if (member != nullptr && lastMember_ == member) calleeBase = lastMemberBase_;
    }
    std::vector<Type> args;
    args.reserve(n.args.size());
    bool spreadArgs = false;
    for (const auto& a : n.args) {
        if (dynamic_cast<const ast::SpreadElement*>(a.get())) spreadArgs = true;
        args.push_back(expr(*a));
    }
    // This site's contribution to the constructor's parameters (ctor_ident.h).
    if (ident != nullptr && mod_.ctors.isClassName(ident->name)) {
        constructSite(ident->name, args, spreadArgs);
    } else {
        constructUnbounded(*n.callee, calleeBase, args, spreadArgs);
    }
    // `new Float64Array(...)` on the UNSHADOWED name is the builtin: assigning
    // to a builtin global is a compile error, so the only way the name can
    // mean anything else is a program binding — which `resolvesToUserBinding`
    // sees, module scope and imports included. The argument forms all produce
    // a view with this element kind, so the arguments do not matter here.
    if (ident != nullptr && !resolvesToUserBinding(ident->name)) {
        if (ident->name == "Float64Array") return Type::typedArray(TypedArrayElem::Float64);
        if (ident->name == "Float32Array") return Type::typedArray(TypedArrayElem::Float32);
        if (ident->name == "Int32Array") return Type::typedArray(TypedArrayElem::Int32);
        if (ident->name == "Uint32Array") return Type::typedArray(TypedArrayElem::Uint32);
        if (ident->name == "Int16Array") return Type::typedArray(TypedArrayElem::Int16);
        if (ident->name == "Uint16Array") return Type::typedArray(TypedArrayElem::Uint16);
        if (ident->name == "Int8Array") return Type::typedArray(TypedArrayElem::Int8);
        if (ident->name == "Uint8Array") return Type::typedArray(TypedArrayElem::Uint8);
        if (ident->name == "Uint8ClampedArray") return Type::typedArray(TypedArrayElem::Uint8Clamped);
        if (ident->name == "Array") return Type::array();
    }
    const ShapeClassId cls = ident != nullptr ? constructorShape(ident->name) : kNoShapeClass;
    if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
    return Type::object(cls);
}

}  // namespace bronze::types
