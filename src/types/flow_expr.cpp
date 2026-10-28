// The expression half of flow analysis: what type an expression produces,
// which callee a call site contributes an argument to, and which shape class
// a literal or a `new` interns. The statement walk is the other half and
// lives in flow.cpp; the seam is argued in flow_analyzer.h.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "types/flow_analyzer.h"
#include "types/math_builtins.h"
#include "types/operator_types.h"
#include "types/walk.h"

namespace bronze::types {
namespace {

bool isNonClassExpr(const ast::Expr* e) {
    if (e == nullptr) return false;
    if (const auto* id = ast::exactAs<const ast::Ident*>(e)) {
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
    return ast::exactAs<const ast::ArrayLit*>(e) || ast::exactAs<const ast::ObjectLit*>(e) ||
           ast::exactAs<const ast::NumberLit*>(e) || ast::exactAs<const ast::StringLit*>(e) ||
           ast::exactAs<const ast::BoolLit*>(e) || ast::exactAs<const ast::RegExpLit*>(e);
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

// `BRONZE_NO_PARAM_CLASS_GUESS=1` stops the parameter shape claim being made at
// all, which puts every method parameter's property sites back on the inline
// cache. Compile-time, like the static-shape and family-guard seams, because
// what it gates is a SHAPE OF EMITTED CODE.
bool paramClassGuessOff() {
    static const bool off = std::getenv("BRONZE_NO_PARAM_CLASS_GUESS") != nullptr;
    return off;
}

// This site's contribution to what class parameter `i` receives.
//
// Only an argument that NAMES a class says anything: a `Dynamic` argument is
// the absence of an observation, not an observation of absence, and the guard
// this feeds decides the question at run time for whatever the argument
// actually is. Two callers naming different classes leave an object with no
// class, which is how "the callers disagree" is spelled, and no claim is made.
void contributeShape(ModuleContext& mod, uint32_t index, const MethodInfo& target, size_t i,
                     const std::vector<Type>& args) {
    if (paramClassGuessOff()) return;
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

}  // namespace

// ---- expressions -------------------------------------------------------

Type FlowAnalyzer::expr(const ast::Expr& e) {
    const Type t = exprKind(e);
    if (record_) {
        Type rec = t;
        if (rec.arrayElementsUndecided()) rec = Type::array();
        if (rec.functionReturnsNever() || rec.functionReturnsNumber()) rec = Type::function();
        if (rec.isDynamicNumOrUndef()) rec = Type::dynamic();
        mod_.result->exprTypes[&e] = rec;
    } else if (!mod_.auditSettled) {
        // Every probe round: the audit is part of the outer fixpoint and has to
        // see the types each round produced. On the final recording pass the
        // audit has already settled and unreached methods have been widened to
        // dynamic parameters, so observing inside inner probe walks there would
        // pollute the settled audit.
        mod_.fieldAudit.observe(&e, t);
    }
    return t;
}

Type FlowAnalyzer::exprKind(const ast::Expr& e) {
    if (ast::exactAs<const ast::NumberLit*>(&e)) return Type::number();
    // DYNAMIC, and deliberately not `number`: the lattice has no BigInt
    // element, and a BigInt typed as one would licence an f64 fast path to
    // read a heap pointer's bits as a double. Dynamic is the designed
    // fallback and the only answer here that cannot be wrong.
    if (ast::exactAs<const ast::BigIntLit*>(&e)) return Type::dynamic();
    if (ast::exactAs<const ast::StringLit*>(&e)) return Type::string();
    // A regular expression literal is an OBJECT, and inference has no shape
    // class for one: its members are `RegExp.prototype`'s accessors and
    // methods, reached through the runtime's ordinary property path.
    if (ast::exactAs<const ast::RegExpLit*>(&e)) return Type::dynamic();
    // A template is a string whatever its substitutions produce, since
    // every one of them goes through ToString. The substitutions are
    // still analysed — they are ordinary expressions and may write
    // bindings.
    if (const auto* t = ast::exactAs<const ast::TemplateLit*>(&e)) {
        for (const auto& sub : t->exprs) expr(*sub);
        return Type::string();
    }
    if (ast::exactAs<const ast::BoolLit*>(&e)) return Type::boolean();
    if (ast::exactAs<const ast::NullLit*>(&e)) return Type::null();
    if (ast::exactAs<const ast::UndefinedLit*>(&e)) return Type::undefined();
    // `this` is the caller's receiver. Inside a class body the DECLARATION
    // names the object kind that receiver is meant to be, and `Scope::thisClass`
    // carries it — see the standing invariant recorded there for why an
    // optimistic answer is safe, and what would stop making it safe.
    if (ast::exactAs<const ast::ThisExpr*>(&e)) {
        // NOT BUILT HERE. The declaration says what the receiver is MEANT to
        // be; `Vector3.prototype.add.call(x)` says what it can actually be, and
        // nothing in this file can see that call. The identity is still worth
        // having — a shape guard checks it, and a wrong guess costs a miss —
        // but a claim about what is INSIDE the object is not guarded by
        // anything, so `this.x` may not be typed `number` on this evidence.
        return scope_.thisClass == kNoShapeClass ? Type::dynamic()
                                                 : Type::objectNotBuiltHere(scope_.thisClass);
    }
    if (const auto* id = ast::exactAs<const ast::Ident*>(&e)) {
        if (!suppressObjectLitSetterReach_ && !scope_.isCell(id->name)) {
            if (const auto it = scope_.objectLitMethodAliases.find(id->name);
                it != scope_.objectLitMethodAliases.end()) {
                if (InferMemo* memo = mod_.rec()) {
                    memo->noteObjectLitTable(mod_.objectLitTableVersion);
                }
                for (const auto& [key, vec] : mod_.objectLitSetters) {
                    if (key.second == it->second) {
                        for (const uint32_t sIdx : vec) {
                            mod_.contribute(InferMemo::Contrib::SetterReached, sIdx, 0,
                                            Type::dynamic());
                        }
                    }
                }
            }
        }
        const Type t = lookup(id->name);
        noteIdentRefusal(*id, t);
        return t;
    }
    if (const auto* u = ast::exactAs<const ast::Unary*>(&e)) return unary(*u);
    if (const auto* b = ast::exactAs<const ast::Binary*>(&e)) return binary(*b);
    if (const auto* t = ast::exactAs<const ast::Ternary*>(&e)) return ternary(*t);
    if (const auto* m = ast::exactAs<const ast::MemberAccess*>(&e)) {
        const bool suppressHere = suppressObjectLitSetterReach_;
        suppressObjectLitSetterReach_ = false;
        const Type base = expr(*m->object);
        lastMember_ = m;
        lastMemberBase_ = base;
        if (base.is(TypeKind::Never)) return Type::never();
        const bool savedSuppress = suppressObjectLitSetterReach_;
        suppressObjectLitSetterReach_ = suppressHere;
        noteStaticMethodAccess(*m, base);
        suppressObjectLitSetterReach_ = savedSuppress;
        if (!m->optional && !m->isPrivate && mod_.valueFlow) {
            if (const auto* id = ast::exactAs<const ast::Ident*>(m->object.get())) {
                if (mod_.ctors.isClassName(id->name)) {
                    const std::string key = id->name + "." + m->property;
                    const auto it = mod_.moduleBindings.find(key);
                    const bool present = it != mod_.moduleBindings.end();
                    if (InferMemo* memo = mod_.rec()) {
                        memo->noteBinding(key, present, present ? it->second : Type::dynamic());
                    }
                    if (present && it->second.is(TypeKind::Object) &&
                        it->second.shapeClass() != kNoShapeClass) {
                        return Type::objectIdentityOnly(it->second.shapeClass());
                    }
                }
            }
        }
        // The Math VALUE properties are numbers on the pristine builtin —
        // 21.3.1 lists exactly these eight, all non-writable and
        // non-configurable, though what carries the proof here is the
        // program-wide pristine bit, not the attributes.
        if (!m->optional && isPristineMathBase(*m->object) &&
            isMathValueProperty(m->property)) {
            if (record_) mod_.result->pristineMathConstants.insert(&e);
            return Type::number();
        }
        if (!m->optional && !m->isPrivate) {
            const Type hostPropT = classifyHostMemberReadType(
                *m,
                [this](const std::string& id) { return resolvesToUserBinding(id); },
                [this](const std::string& id) {
                    const auto it = scope_.hostObjectBindings.find(id);
                    return it != scope_.hostObjectBindings.end() ? it->second
                                                                 : HostObjectKind::None;
                },
                mod_.mathPristine);
            if (!hostPropT.is(TypeKind::Dynamic)) return hostPropT;
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
            if (mod_.result->classLayouts.byShapeClass(base.shapeClass()) == nullptr) {
                if (!base.identityOnly()) {
                    const Type field = mod_.objectLitFieldType(base.shapeClass(), m->property);
                    const bool clean =
                        mod_.arrayElemShapeClasses.count(base.shapeClass()) != 0 &&
                        mod_.fieldAudit.objectLitFieldClean(base.shapeClass(), m->property);
                    if (InferMemo* memo = mod_.rec()) {
                        memo->noteField(base.shapeClass(), m->property, field, clean);
                    }
                    if (mod_.arrayElemShapeClasses.count(base.shapeClass()) != 0) {
                        if (field.is(TypeKind::Never)) return Type::never();
                        if (field.is(TypeKind::Number)) {
                            if (record_) ++mod_.result->fieldAudit.numberFieldReads;
                            if (!clean) {
                                if (record_) ++mod_.result->fieldAudit.refusedByAudit;
                                return Type::dynamic();
                            }
                            if (record_) mod_.result->provenFieldReads.insert(m);
                            return Type::number();
                        }
                    }
                }
                return Type::dynamic();
            }
            Type field =
                mod_.result->classLayouts.fieldTypeOf(base.shapeClass(), m->property);
            if (InferMemo* memo = mod_.rec()) {
                memo->noteField(base.shapeClass(), m->property, field,
                                mod_.fieldAudit.numberCleanFor(base.shapeClass(), m->property));
            }
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
            if (field.is(TypeKind::Array) &&
                (field.arrayElemClass() != kNoShapeClass || field.arrayElementsUndecided())) {
                return field;
            }
            if (field.is(TypeKind::Dynamic) &&
                mod_.fieldAudit.numberCleanFor(base.shapeClass(), m->property)) {
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
            const bool classFamilyValidated =
                mod_.methodParamTypes &&
                mod_.result->classLayouts.familyMemberOf(base.shapeClass()) != nullptr;
            if (!base.builtHere() && !classFamilyValidated) {
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
            if (!mod_.fieldAudit.numberCleanFor(base.shapeClass(), m->property)) {
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
    if (const auto* ix = ast::exactAs<const ast::IndexAccess*>(&e)) {
        const Type base = expr(*ix->object);
        const Type idx = expr(*ix->index);
        if (base.is(TypeKind::Never) || idx.is(TypeKind::Never)) return Type::never();
        if (!ix->optional && base.is(TypeKind::Array)) {
            if (base.arrayElementsUndecided()) return Type::never();
            if (base.arrayElemClass() != kNoShapeClass) {
                const ShapeClassId cls = base.arrayElemClass();
                return mod_.result->classLayouts.isExtended(cls)
                           ? Type::objectIdentityOnly(cls)
                           : Type::objectNotBuiltHere(cls);
            }
        }
        if (!ix->optional && idx.is(TypeKind::Number) &&
            ((base.is(TypeKind::TypedArray) && base.typedArrayElemRaw() != kNoTypedArrayElem) ||
             base.arrayElementsPinned())) {
            lastNumericElem_ = ix;
        }
        if (!ix->optional) {
            if (const auto* id = ast::exactAs<const ast::Ident*>(ix->object.get())) {
                if (!scope_.isCell(id->name)) {
                    const auto it = scope_.dictElemTypes.find(id->name);
                    if (it != scope_.dictElemTypes.end() && it->second.is(TypeKind::Object) &&
                        !it->second.identityOnly() && it->second.shapeClass() != kNoShapeClass &&
                        mod_.result->classLayouts.byShapeClass(it->second.shapeClass()) == nullptr) {
                        return it->second;
                    }
                }
            }
        }
        return Type::dynamic();
    }
    if (const auto* c = ast::exactAs<const ast::Call*>(&e)) return call(*c);
    if (const auto* n = ast::exactAs<const ast::NewExpr*>(&e)) return newExpr(*n);
    if (ast::exactAs<const ast::NewTargetExpr*>(&e)) return Type::dynamic();
    if (ast::exactAs<const ast::ImportMetaExpr*>(&e)) return Type::dynamic();
    if (const auto* tt = ast::exactAs<const ast::TaggedTemplate*>(&e)) {
        expr(*tt->tag);
        for (const auto& el : tt->templateLit->exprs) expr(*el);
        return Type::dynamic();
    }
    if (const auto* o = ast::exactAs<const ast::ObjectLit*>(&e)) return objectLit(*o);
    if (const auto* a = ast::exactAs<const ast::ArrayLit*>(&e)) {
        bool canTrackElem = true;
        bool allNever = true;
        ShapeClassId commonCls = kNoShapeClass;
        for (const auto& el : a->elements) {
            if (!el) {
                canTrackElem = false;
                continue;
            }
            if (ast::exactAs<const ast::SpreadElement*>(el.get())) canTrackElem = false;
            const Type elT = expr(*el);
            if (!canTrackElem) continue;
            if (elT.is(TypeKind::Never)) continue;
            allNever = false;
            if (elT.is(TypeKind::Object) && elT.shapeClass() != kNoShapeClass) {
                if (commonCls == kNoShapeClass) {
                    commonCls = elT.shapeClass();
                } else if (commonCls != elT.shapeClass()) {
                    canTrackElem = false;
                }
            } else {
                canTrackElem = false;
            }
        }
        if (canTrackElem) {
            if (allNever) return Type::arrayOfNever();
            if (commonCls != kNoShapeClass) {
                if (mod_.arrayElemShapeClasses.insert(commonCls).second) ++mod_.persistentWrites;
                return Type::arrayOfObject(commonCls);
            }
        }
        return Type::array();
    }
    if (const auto* sc = ast::exactAs<const ast::SuperCall*>(&e)) return superCall(*sc);
    if (ast::exactAs<const ast::SuperMember*>(&e)) return Type::dynamic();
    if (const auto* ce = ast::exactAs<const ast::ClassExpr*>(&e)) {
        analyzeClassBody(ce->name, ce->methods);
        return Type::function();
    }
    // The value of a `yield` is the argument of the `next(v)` that resumed the
    // generator, which comes from outside this compilation entirely. Its
    // operand is still analysed: it is ordinary code that runs here.
    if (const auto* y = ast::exactAs<const ast::YieldExpr*>(&e)) {
        expr(*y->argument);
        return Type::dynamic();
    }
    if (const auto* di = ast::exactAs<const ast::DynamicImportExpr*>(&e)) {
        if (di->specifier) expr(*di->specifier);
        return Type::dynamic();
    }
    // A spread contributes its argument's effects and nothing about the
    // container's element types — there is no element type here to prove.
    if (const auto* sp = ast::exactAs<const ast::SpreadElement*>(&e)) {
        expr(*sp->argument);
        return Type::dynamic();
    }
    // Every name a destructuring assignment writes becomes dynamic: the pieces
    // come out of an indexed or keyed read, and this pass tracks no element or
    // property types to say anything narrower. Assigning rather than ignoring
    // is the point — a name proven numeric before must not stay numeric across
    // it.
    if (const auto* da = ast::exactAs<const ast::DestructuringAssign*>(&e)) {
        const Type value = expr(*da->value);
        // The pattern's own code — computed keys, defaults, and the object and
        // key of a member target — runs after the value, and a call in it is a
        // call site like any other: `[h[key('x')]] = xs` passes a string to
        // `key`, and a proof that never saw it types the parameter a number.
        patternDefaults(*da->pattern);
        for (const auto& name : ast::patternBoundNames(*da->pattern)) {
            assign(name, Type::dynamic());
        }
        return value;
    }
    if (const auto* f = ast::exactAs<const ast::FunctionExpr*>(&e)) {
        if (!record_ && !mod_.auditSettled && mod_.interprocIdent && mod_.valueFlow) {
            const uint32_t sIdx = mod_.methods.setterIndexOfNode(f);
            if (sIdx != kNoMethod) {
                const SetterInfo& s = mod_.methods.setters()[sIdx];
                if (InferMemo* memo = mod_.rec()) memo->noteSetter(sIdx, s.version);
                if (!s.reached) return Type::function();
            }
        }
        // 15.3.4: an arrow has no `this` binding of its own and resolves the
        // name in the enclosing scope, so it inherits whatever receiver is in
        // hand. A non-arrow function expression binds its own, which the
        // caller supplies and no declaration here can name.
        const Type ret = analyzeNested(*f, f->name, f->params, f->body, f->span,
                                       f->isGenerator || f->isAsync,
                                       f->isArrow ? scope_.thisClass : kNoShapeClass);
        if (!f->isGenerator && !f->isAsync) {
            if (ret.is(TypeKind::Never)) return Type::functionReturningNever();
            if (ret.is(TypeKind::Number)) return Type::functionReturningNumber();
        }
        return Type::function();
    }
    fail(e.span, "saw an unknown expression node kind");
    return Type::dynamic();
}

// Evaluates `e` and, when `e` is a numeric TypedArray (Int8..Float64) or
// pinned-array element read at a Number index, upgrades its `Dynamic` answer to
// `Type::undefined()` for operator-result typing. By ECMA-262 10.4.5.4 such a
// read produces only a Number or `undefined` (on an invalid/out-of-bounds
// index), never a String, Object, or BigInt — and `Undefined` has the exact
// operator coercion behaviour of `Number | undefined` in `operator_types.cpp`
// (`isNumericPrimitive` and `isNeverBigInt`).
Type FlowAnalyzer::exprForOperator(const ast::Expr& e) {
    const Type t = expr(e);
    if (t.is(TypeKind::Dynamic)) {
        const auto* ix = ast::exactAs<const ast::IndexAccess*>(&e);
        if (ix != nullptr && lastNumericElem_ == ix) {
            return Type::undefined();
        }
        if (const auto* m = ast::exactAs<const ast::MemberAccess*>(&e)) {
            if (lastMember_ == m && !m->optional && !m->isPrivate && mod_.interprocIdent) {
                if (const ClassLayout* cl = receiverClass(lastMemberBase_)) {
                    const std::string backing =
                        mod_.methods.getterBackingField(cl->name, m->property);
                    if (!backing.empty()) {
                        const Type ft =
                            mod_.result->classLayouts.fieldTypeOf(cl->shapeClass, backing);
                        const bool clean = mod_.fieldAudit.numberCleanFor(cl->shapeClass, backing);
                        if (InferMemo* memo = mod_.rec()) {
                            memo->noteField(cl->shapeClass, backing, ft, clean);
                        }
                        if (ft.is(TypeKind::Never)) return Type::never();
                        if ((ft.is(TypeKind::Number) || ft.is(TypeKind::Dynamic)) && clean) {
                            return Type::number();
                        }
                    }
                }
            }
        }
    }
    return t;
}

Type FlowAnalyzer::call(const ast::Call& c) {
    const auto* member = ast::exactAs<const ast::MemberAccess*>(c.callee.get());
    const auto* calleeId = ast::exactAs<const ast::Ident*>(c.callee.get());
    const bool suppressSetter =
        !c.optional &&
        ((member != nullptr && !member->optional && !member->isPrivate && !member->property.empty()) ||
         (calleeId != nullptr && !scope_.isCell(calleeId->name) &&
          scope_.objectLitMethodAliases.count(calleeId->name) != 0));
    const bool savedSuppressSetter = suppressObjectLitSetterReach_;
    if (suppressSetter) suppressObjectLitSetterReach_ = true;
    const Type calleeType = expr(*c.callee);
    suppressObjectLitSetterReach_ = savedSuppressSetter;
    const bool hadMemberBase = (member != nullptr && lastMember_ == member);
    const Type receiver = hadMemberBase ? lastMemberBase_ : Type::dynamic();
    std::vector<Type> args;
    args.reserve(c.args.size());
    bool spreadArgs = false;
    for (const auto& a : c.args) {
        if (ast::exactAs<const ast::SpreadElement*>(a.get())) spreadArgs = true;
        args.push_back(expr(*a));
    }
    const std::optional<Type> objLitSetterRet =
        callObjectLitSetter(c, member, receiver, args, spreadArgs);

    // `Math.<fn>(...)` on the pristine builtin: every OWN function property
    // of 21.3 returns a Number for ANY arguments (a BigInt argument throws
    // before a value exists, which claims nothing). The list is the own
    // methods only — an INHERITED call like `Math.toString()` reaches
    // Object.prototype and is deliberately absent.
    if (!c.optional && mathCallReturnsNumber(c)) {
        if (record_) mod_.result->pristineMathCalls.insert(&c);
        return Type::number();
    }
    if (!c.optional) {
        if (const auto* id = ast::exactAs<const ast::Ident*>(c.callee.get())) {
            if (!scope_.isCell(id->name)) {
                if (const auto it = scope_.pristineMathFns.find(id->name);
                    it != scope_.pristineMathFns.end()) {
                    // `const cos = Math.cos; cos(x)`: the binding is the
                    // pristine builtin for as long as it is in scope, so the
                    // call is `Math.cos(x)` with `this` undefined, which no
                    // member of 21.3 reads.
                    if (record_) mod_.result->pristineMathAliasCalls.emplace(&c, it->second);
                    return Type::number();
                }
            }
        }
        if (isHostCallReturningNumber(
                c, [this](const std::string& id) { return resolvesToUserBinding(id); },
                mod_.mathPristine)) {
            return Type::number();
        }
    }

    bool pinnedReturn = false;
    if (!c.optional && mod_.pins != nullptr) {
        const auto* ident = ast::exactAs<const ast::Ident*>(c.callee.get());
        if (ident != nullptr && mod_.pins->returnPinned(ident->name)) pinnedReturn = true;
    }

    // `recv.m(...)`: the call sites a class METHOD has, enumerated through the
    // receiver's class rather than through the callee's name.
    if (member != nullptr && !c.optional && hadMemberBase && receiver.is(TypeKind::Array)) {
        if (member->property == "push" || member->property == "unshift") {
            if (const auto* id = ast::exactAs<const ast::Ident*>(member->object.get())) {
                if (scope_.env.count(id->name) != 0 || scope_.cells.count(id->name) != 0) {
                    Type updated = receiver;
                    if (spreadArgs) {
                        updated = Type::array();
                    } else {
                        for (const Type& at : args) {
                            if (at.is(TypeKind::Never)) {
                                updated = join(updated, Type::arrayOfNever());
                            } else if (at.is(TypeKind::Object) && at.shapeClass() != kNoShapeClass) {
                                if (mod_.arrayElemShapeClasses.insert(at.shapeClass()).second) {
                                    ++mod_.persistentWrites;
                                }
                                updated = join(updated, Type::arrayOfObject(at.shapeClass()));
                            } else {
                                updated = Type::array();
                            }
                        }
                    }
                    if (updated != receiver) assign(id->name, updated);
                }
            }
        } else if ((member->property == "shift" || member->property == "pop") &&
                    args.empty() && !spreadArgs &&
                    !mod_.methods.isMethodName(member->property)) {
            if (receiver.arrayElementsUndecided()) return Type::never();
            if (receiver.arrayElemClass() != kNoShapeClass) {
                const ShapeClassId cls = receiver.arrayElemClass();
                return mod_.result->classLayouts.isExtended(cls)
                           ? Type::objectIdentityOnly(cls)
                           : Type::objectNotBuiltHere(cls);
            }
        }
    }
    if (member != nullptr) {
        if (!c.optional && member->property == "freeze" && !args.empty()) {
            if (const auto* objId = ast::exactAs<const ast::Ident*>(member->object.get())) {
                if (objId->name == "Object" && !resolvesToUserBinding("Object")) {
                    return args[0];
                }
            }
        }
        if (member->property == "stringify" && mod_.interprocIdent && !args.empty()) {
            const Type arg0 = args[0];
            bool canHaveToJSON =
                !arg0.is(TypeKind::Never) && !arg0.is(TypeKind::Number) &&
                !arg0.is(TypeKind::String) && !arg0.is(TypeKind::Bool) &&
                !arg0.is(TypeKind::Null) && !arg0.is(TypeKind::Undefined) &&
                !arg0.is(TypeKind::TypedArray);
            if (canHaveToJSON && arg0.is(TypeKind::Object) && arg0.shapeClass() != kNoShapeClass &&
                mod_.result->classLayouts.byShapeClass(arg0.shapeClass()) == nullptr &&
                mod_.objectLitUnclosed.count(arg0.shapeClass()) == 0) {
                const auto fIt = mod_.objectLitFields.find(arg0.shapeClass());
                bool hasComplexField = false;
                if (fIt != mod_.objectLitFields.end()) {
                    for (const auto& [fk, fv] : fIt->second) {
                        if (fv.is(TypeKind::Object) || fv.is(TypeKind::Array) ||
                            fv.is(TypeKind::Dynamic)) {
                            hasComplexField = true;
                            break;
                        }
                    }
                }
                if (!hasComplexField) canHaveToJSON = false;
            }
            if (canHaveToJSON && !c.args.empty()) {
                if (const auto* argMem = ast::exactAs<const ast::MemberAccess*>(c.args[0].get())) {
                    if (!argMem->optional && !argMem->isPrivate &&
                        mod_.fieldAudit.propertyOnlyPlainObjects(
                            argMem->property, mod_.objectLitUnclosed, mod_.objectLitFields)) {
                        canHaveToJSON = false;
                    }
                }
            }
            if (canHaveToJSON) {
                if (const auto* decls = mod_.methods.declarationsOf("toJSON")) {
                    for (const uint32_t idx : *decls) {
                        const MethodInfo& target = mod_.methods.methods()[idx];
                        if (mod_.ctorParamTypes) {
                            const uint32_t cIdx = mod_.ctors.classIndexOf(target.className);
                            if (InferMemo* memo = mod_.rec(); memo != nullptr && cIdx != kNoCtor) {
                                memo->noteClassReach(cIdx, mod_.ctors.classReach()[cIdx].version);
                            }
                            if (!mod_.ctors.isClassReached(target.className)) continue;
                        }
                        mod_.contribute(InferMemo::Contrib::MethodReached, idx, 0, Type::never());
                    }
                }
            }
        }
        if (member->property == "call" || member->property == "apply" || member->property == "bind") {
            if (const auto* fnMem = ast::exactAs<const ast::MemberAccess*>(member->object.get())) {
                if (const auto* protoMem = ast::exactAs<const ast::MemberAccess*>(fnMem->object.get())) {
                    if (protoMem->property == "prototype") {
                        if (const auto* clsId = ast::exactAs<const ast::Ident*>(protoMem->object.get())) {
                            if (mod_.ctors.isClassName(clsId->name)) markClassReached(clsId->name);
                        }
                    }
                }
            }
        }
        if (objLitSetterRet.has_value()) return *objLitSetterRet;
        if (hadMemberBase && isNonClassExpr(member->object.get())) return Type::dynamic();
        return methodCall(member->property, receiver, args, spreadArgs);
    }
    if (objLitSetterRet.has_value() && !pinnedReturn) return *objLitSetterRet;
    // `super.m(...)`. The receiver is this method's `this` — a subclass instance
    // — but the LOOKUP starts at the enclosing class's base, which is the one
    // thing about the dispatch that is decided statically. So the contribution
    // goes to whatever `m` that base would find, and to the overrides below it,
    // exactly as an ordinary call on a base-typed receiver does.
    if (const auto* sup = ast::exactAs<const ast::SuperMember*>(c.callee.get())) {
        // A static element's `super.m(...)` reaches the base's STATIC `m`,
        // which the layouts below do not describe: they hold instance
        // methods, and typing the call by one of those would be typing it by
        // the wrong function.
        if (sup->fromStatic || sup->fromObjectLiteral || sup->propertyExpr != nullptr) return Type::dynamic();
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

    if (const auto it = mod_.nestedFnByCall.find(&c); it != mod_.nestedFnByCall.end()) {
        const uint32_t nIdx = it->second;
        const NestedFnInfo& callee = mod_.nestedFns[nIdx];
        for (size_t i = 0; i < callee.observedParams.size(); ++i) {
            const Type at = i < args.size() ? args[i] : Type::undefined();
            mod_.contribute(InferMemo::Contrib::NestedFnParam, nIdx, static_cast<uint32_t>(i), at);
        }
        if (pinnedReturn) return Type::number();
        if (InferMemo* memo = mod_.rec()) memo->noteNestedFn(nIdx, callee.version);
        if (!callee.signature.returnType.is(TypeKind::Never)) return callee.signature.returnType;
        if (!callee.bodyReturn.is(TypeKind::Never)) return callee.bodyReturn;
    }

    const uint32_t index = calleeType.functionIndex();
    if (index == kNoFunctionIndex) {
        if (pinnedReturn) return Type::number();
        if (calleeType.is(TypeKind::Never) || (!c.optional && calleeType.functionReturnsNever())) {
            return Type::never();
        }
        if (!c.optional && calleeType.functionReturnsNumber()) return Type::number();
        return Type::dynamic();
    }

    FunctionInfo& callee = mod_.functions[index];
    if (!callee.directCallable) {
        if (pinnedReturn) return Type::number();
        if (InferMemo* memo = mod_.rec()) memo->noteFunction(index, callee.version);
        return callee.bodyReturn.is(TypeKind::Never) || callee.bodyReturn.is(TypeKind::Number)
                   ? callee.bodyReturn
                   : Type::dynamic();
    }

    // This site's contribution to the callee's parameters. A missing
    // argument is `undefined`, exactly as the call would deliver it.
    for (size_t i = 0; i < callee.observedParams.size(); ++i) {
        const Type at = i < args.size() ? args[i] : Type::undefined();
        mod_.contribute(InferMemo::Contrib::FnParam, index, static_cast<uint32_t>(i), at);
    }
    // The pin wins over the join for the RESULT — the invocation's promise is
    // the stronger statement, and it is the one the callee's typed entry was
    // built from (`applySignaturePins`). The contribution above happened either
    // way.
    if (pinnedReturn) return Type::number();
    if (InferMemo* memo = mod_.rec()) memo->noteFunction(index, callee.version);
    return !callee.signature.returnType.is(TypeKind::Never) ? callee.signature.returnType
                                                            : callee.bodyReturn;
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
    // Nothing has reached this receiver yet. The call-graph fixpoint has not
    // finished; a round that read this as "unproven" would be poisoning the name
    // on evidence that does not exist.
    if (receiver.is(TypeKind::Never)) return Type::never();
    // No class declares a method by this name, so no parameter of any modelled
    // method is at stake — this is a call on a builtin, on host code, or on a
    // function a property happens to hold.
    if (!mod_.methods.isMethodName(name)) return Type::dynamic();

    if (receiver.is(TypeKind::Array) || receiver.is(TypeKind::TypedArray) ||
        receiver.is(TypeKind::Number) || receiver.is(TypeKind::String) ||
        receiver.is(TypeKind::Bool) || receiver.is(TypeKind::Null) ||
        receiver.is(TypeKind::Undefined) ||
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
                const MethodInfo& target = mod_.methods.methods()[index];
                if (mod_.ctorParamTypes) {
                    const uint32_t cIdx = mod_.ctors.classIndexOf(target.className);
                    if (InferMemo* memo = mod_.rec(); memo != nullptr && cIdx != kNoCtor) {
                        memo->noteClassReach(cIdx, mod_.ctors.classReach()[cIdx].version);
                    }
                    if (!mod_.ctors.isClassReached(target.className)) continue;
                }
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
        for (const uint32_t index : *decls) {
            MethodInfo& target = mod_.methods.methods()[index];
            if (mod_.ctorParamTypes) {
                const uint32_t cIdx = mod_.ctors.classIndexOf(target.className);
                if (InferMemo* memo = mod_.rec(); memo != nullptr && cIdx != kNoCtor) {
                    memo->noteClassReach(cIdx, mod_.ctors.classReach()[cIdx].version);
                }
                if (!mod_.ctors.isClassReached(target.className)) continue;
            }
            // A target declaring more parameters than this site passes is
            // reached all the same: the call does not pick its method by
            // arity, so each omitted parameter without a default receives
            // `undefined` here (contributeMethodArg). Skipping such a target
            // left its join to the other sites, and a parameter typed `number`
            // from them read `undefined` as NaN.
            contributeArgs(index, target);
        }
        if (record_) ++mod_.unboundedMethodCalls;
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
