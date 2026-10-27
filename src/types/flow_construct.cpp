// The constructor and `new` half of flow analysis: `new` expressions, constructor
// call sites, and argument propagation into constructor parameters.
// The expression walk is in flow_expr.cpp; the seam is argued in flow_analyzer.h.

#include "ast/queries.h"
#include "types/flow_analyzer.h"
#include "types/math_builtins.h"
#include "types/operator_types.h"
#include "types/relational_prover.h"
#include "types/walk.h"

namespace bronze::types {

void FlowAnalyzer::markClassReached(const std::string& className) {
    if (!mod_.ctorParamTypes) return;
    std::vector<std::string> chain;
    mod_.ctors.ancestorsOf(className, chain);
    for (const auto& name : chain) {
        const uint32_t idx = mod_.ctors.classIndexOf(name);
        if (idx != kNoCtor) {
            mod_.contribute(InferMemo::Contrib::ClassReached, idx, 0, Type::never());
        }
    }
}

// One `new <a class name>(...)`, and every `super(...)` that reaches one.
//
// The site's arguments join into the parameters of whatever constructor the
// name positionally reaches — the class's own, or, when it declares none, the
// one its base reaches, because the implicit constructor forwards everything.
void FlowAnalyzer::constructSite(const std::string& className, const std::vector<Type>& args,
                                 bool spreadArgs) {
    if (!mod_.ctorParamTypes) return;
    markClassReached(className);
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
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&callee)) {
        const auto* thenId = dynamic_cast<const ast::Ident*>(t->thenExpr.get());
        const auto* elseId = dynamic_cast<const ast::Ident*>(t->elseExpr.get());
        if (thenId != nullptr && elseId != nullptr &&
            mod_.ctors.isClassName(thenId->name) && mod_.ctors.isClassName(elseId->name)) {
            constructSite(thenId->name, args, spreadArgs);
            constructSite(elseId->name, args, spreadArgs);
            return;
        }
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&callee)) {
        if (m->property == "constructor" && !m->optional) {
            if (calleeBase.is(TypeKind::Never) || calleeBase.is(TypeKind::TypedArray) ||
                calleeBase.is(TypeKind::Array) || calleeBase.is(TypeKind::Number) ||
                calleeBase.is(TypeKind::String) || calleeBase.is(TypeKind::Bool)) {
                if (record_) ++mod_.unnamedNewIgnored;
                return;
            }
            if (const auto* inner = dynamic_cast<const ast::MemberAccess*>(m->object.get())) {
                if (inner->property == "array" || inner->property == "data" ||
                    inner->property == "buffer" || inner->property == "elements") {
                    if (record_) ++mod_.unnamedNewIgnored;
                    return;
                }
            }
            if (const ClassLayout* cls = receiverClass(calleeBase)) {
                std::vector<uint32_t> targets;
                mod_.ctors.subtreeOf(cls->name, targets);
                for (const uint32_t target : targets) {
                    const auto& info = mod_.ctors.ctors()[target];
                    const uint32_t classIdx = mod_.ctors.classIndexOf(info.className);
                    if (InferMemo* memo = mod_.rec()) {
                        memo->noteCtor(target, info.version, mod_.ctorPoison.poisons(info.className));
                        if (classIdx != kNoCtor) {
                            memo->noteClassReach(classIdx, mod_.ctors.classReach()[classIdx].version);
                        }
                    }
                    if (!mod_.ctors.isClassReached(info.className)) continue;
                    contributeCtorArgs(target, args, spreadArgs);
                }
                if (record_) ++mod_.unnamedNewSubtree;
                return;
            }
            if (!mod_.ctorEscapes.valueEscapes) {
                if (record_) ++mod_.unnamedNewIgnored;
                return;
            }
            if (spreadArgs) {
                if (!record_ && !mod_.auditSettled) {
                    mod_.ctorPoison.addAll("an argument list is spread at a `new` whose callee is a value");
                }
                return;
            }
            if (record_) ++mod_.unnamedNewAll;
            for (uint32_t i = 0; i < mod_.ctors.ctors().size(); ++i) {
                const auto& info = mod_.ctors.ctors()[i];
                const uint32_t classIdx = mod_.ctors.classIndexOf(info.className);
                if (InferMemo* memo = mod_.rec()) {
                    memo->noteCtor(i, info.version, mod_.ctorPoison.poisons(info.className));
                    if (classIdx != kNoCtor) {
                        memo->noteClassReach(classIdx, mod_.ctors.classReach()[classIdx].version);
                    }
                }
                if (!mod_.ctors.isClassReached(info.className)) continue;
                contributeCtorArgs(i, args, /*spreadArgs=*/false);
            }
            return;
        }
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
    for (const auto& cr : mod_.ctors.classReach()) {
        if (mod_.ctorPoison.poisons(cr.className)) {
            markClassReached(cr.className);
        }
    }
    if (!mod_.ctorEscapes.valueEscapes) {
        if (record_) ++mod_.unnamedNewIgnored;
        return;
    }
    if (spreadArgs) {
        if (!record_ && !mod_.auditSettled) {
            mod_.ctorPoison.addAll("an argument list is spread at a `new` whose callee is a value");
        }
        return;
    }
    if (record_) ++mod_.unnamedNewAll;
    for (uint32_t i = 0; i < mod_.ctors.ctors().size(); ++i) {
        const auto& info = mod_.ctors.ctors()[i];
        const uint32_t classIdx = mod_.ctors.classIndexOf(info.className);
        if (InferMemo* memo = mod_.rec()) {
            memo->noteCtor(i, info.version, mod_.ctorPoison.poisons(info.className));
            if (classIdx != kNoCtor) {
                memo->noteClassReach(classIdx, mod_.ctors.classReach()[classIdx].version);
            }
        }
        if (!mod_.ctors.isClassReached(info.className)) continue;
        contributeCtorArgs(i, args, /*spreadArgs=*/false);
    }
}

void FlowAnalyzer::contributeCtorArgs(uint32_t ctorIndex, const std::vector<Type>& args,
                                      bool spreadArgs) {
    CtorInfo& info = mod_.ctors.ctors()[ctorIndex];
    if (spreadArgs) {
        if (!record_ && !mod_.auditSettled) {
            mod_.ctorPoison.add(info.className, "an argument list is spread at a construction site");
        }
        return;
    }
    for (size_t i = 0; i < info.observedParams.size(); ++i) {
        if (i < args.size()) {
            // `undefined` at a defaulted position runs the DEFAULT, so the
            // value bound here is not the argument and the argument is not
            // evidence. The default's own type is joined in by the walk that
            // evaluates it (`runParamDefaults`).
            if (info.hasDefault[i] && args[i].is(TypeKind::Undefined)) continue;
            mod_.contribute(InferMemo::Contrib::CtorParam, ctorIndex, static_cast<uint32_t>(i),
                            args[i]);
            continue;
        }
        // A missing argument at a position with no default binds `undefined`,
        // exactly as the construction delivers it.
        if (!info.hasDefault[i]) {
            mod_.contribute(InferMemo::Contrib::CtorParam, ctorIndex, static_cast<uint32_t>(i),
                            Type::undefined());
        }
    }
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
    if (ident == nullptr) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            if (m->property == "constructor" && !m->optional) {
                if (const ClassLayout* cl = receiverClass(calleeBase)) {
                    return Type::objectIdentityOnly(cl->shapeClass);
                }
            }
        }
    }
    const ShapeClassId cls = ident != nullptr ? constructorShape(ident->name) : kNoShapeClass;
    if (record_ && cls != kNoShapeClass) mod_.result->siteShapes[&n] = cls;
    return Type::object(cls);
}

Type FlowAnalyzer::superCall(const ast::SuperCall& sc) {
    std::vector<Type> args;
    args.reserve(sc.args.size());
    bool spreadArgs = false;
    for (const auto& a : sc.args) {
        if (ast::exactAs<const ast::SpreadElement*>(a.get())) spreadArgs = true;
        args.push_back(expr(*a));
    }
    const bool forwarding = scope_.ctorIndex != kNoCtor &&
                            mod_.ctors.ctors()[scope_.ctorIndex].isForwarder;
    if (mod_.ctorParamTypes && !forwarding) {
        const ClassLayout* here = mod_.result->classLayouts.byShapeClass(scope_.thisClass);
        if (here != nullptr && !here->superName.empty()) {
            constructSite(here->superName, args, spreadArgs);
        }
    }
    return Type::dynamic();
}

namespace {

bool collectEqUndefinedNames(const ast::Expr* e, std::vector<std::string>& out) {
    if (e == nullptr) return false;
    if (const auto* b = ast::exactAs<const ast::Binary*>(e)) {
        if (b->op == ast::BinaryOp::LogicalAnd) {
            return collectEqUndefinedNames(b->lhs.get(), out) &&
                   collectEqUndefinedNames(b->rhs.get(), out);
        }
        if (b->op == ast::BinaryOp::StrictEq || b->op == ast::BinaryOp::Eq) {
            auto isUndef = [](const ast::Expr* side) {
                if (ast::exactAs<const ast::UndefinedLit*>(side) != nullptr) return true;
                if (const auto* id = ast::exactAs<const ast::Ident*>(side)) {
                    return id->name == "undefined";
                }
                return false;
            };
            if (const auto* lhsId = ast::exactAs<const ast::Ident*>(b->lhs.get());
                lhsId != nullptr && isUndef(b->rhs.get())) {
                out.push_back(lhsId->name);
                return true;
            }
            if (const auto* rhsId = ast::exactAs<const ast::Ident*>(b->rhs.get());
                rhsId != nullptr && isUndef(b->lhs.get())) {
                out.push_back(rhsId->name);
                return true;
            }
        }
    }
    return false;
}

std::pair<std::string, std::string> extractIsTagGuard(const ast::Expr* e) {
    if (e == nullptr) return {};
    const ast::MemberAccess* m = ast::exactAs<const ast::MemberAccess*>(e);
    const std::string* guardedName = nullptr;
    if (const auto* b = ast::exactAs<const ast::Binary*>(e)) {
        if (b->op == ast::BinaryOp::LogicalAnd) {
            if (const auto* lhsId = ast::exactAs<const ast::Ident*>(b->lhs.get())) {
                guardedName = &lhsId->name;
                m = ast::exactAs<const ast::MemberAccess*>(b->rhs.get());
            }
        }
    }
    if (m == nullptr || m->optional || m->isPrivate || m->property.size() <= 2 ||
        m->property.compare(0, 2, "is") != 0) {
        return {};
    }
    const auto* objId = ast::exactAs<const ast::Ident*>(m->object.get());
    if (objId == nullptr) return {};
    if (guardedName != nullptr && *guardedName != objId->name) return {};
    return {objId->name, m->property};
}

}  // namespace

void FlowAnalyzer::ifStmt(const ast::IfStmt& i, uint32_t depth) {
    expr(*i.condition);
    const Env entry = scope_.env;
    const auto hostBindingsEntry = scope_.hostObjectBindings;
    const ConditionRefinements condRefs = extractConditionRefinements(
        i.condition.get(),
        [this](const std::string& name) { return lookup(name); },
        [this](const std::string& name) { return resolvesToUserBinding(name); },
        mod_.mathPristine);
    const bool thenTerminates = stmtListDefinitelyTerminates(i.thenBody);
    const bool elseTerminates =
        !i.elseBody.empty() && stmtListDefinitelyTerminates(i.elseBody);

    std::string narrowedTagVar;
    if (!i.thenBody.empty() && mod_.interprocIdent) {
        const auto [varName, tagProp] = extractIsTagGuard(i.condition.get());
        if (!varName.empty() && !scope_.isCell(varName)) {
            if (const auto it = scope_.env.find(varName);
                it != scope_.env.end() && it->second.is(TypeKind::Dynamic)) {
                const ClassLayout* uniqueCls = nullptr;
                uint32_t matches = 0;
                for (const auto& cl : mod_.result->classLayouts.all()) {
                    const auto fIt = cl.fieldTypes.find(tagProp);
                    if (fIt != cl.fieldTypes.end() && fIt->second.is(TypeKind::Bool)) {
                        uniqueCls = &cl;
                        ++matches;
                    }
                }
                if (matches == 1 && uniqueCls != nullptr) {
                    it->second = Type::objectIdentityOnly(uniqueCls->shapeClass);
                    narrowedTagVar = varName;
                }
            }
        }
    }
    applyConditionRefinements(scope_.env, condRefs.whenTrue,
                              [this](const std::string& name) { return scope_.isCell(name); });
    if (!i.thenBody.empty()) {
        pushMarker("then", depth + 1);
        scopedStmtList(i.thenBody, depth + 2);
    }
    Env thenEnv = scope_.env;
    const auto hostBindingsThen = scope_.hostObjectBindings;
    if (!narrowedTagVar.empty()) {
        if (const auto it = thenEnv.find(narrowedTagVar); it != thenEnv.end()) {
            if (const auto origIt = entry.find(narrowedTagVar); origIt != entry.end()) {
                it->second = origIt->second;
            }
        }
    }
    scope_.env = entry;
    scope_.hostObjectBindings = hostBindingsEntry;
    applyConditionRefinements(scope_.env, condRefs.whenFalse,
                              [this](const std::string& name) { return scope_.isCell(name); });
    std::vector<std::string> undefNames;
    std::vector<std::string> extraParamNames;
    if (collectEqUndefinedNames(i.condition.get(), undefNames)) {
        for (const auto& name : undefNames) {
            if (scope_.isCell(name)) continue;
            if (const auto it = scope_.env.find(name);
                it != scope_.env.end() && it->second.isDynamicNumOrUndef()) {
                it->second = Type::number();
            }
        }
        if (undefNames.size() >= 2 && !i.elseBody.empty()) {
            for (const auto& pName : facts_.paramNames) {
                if (pName.empty() || scope_.isCell(pName)) continue;
                if (const auto it = scope_.env.find(pName);
                    it != scope_.env.end() && it->second.isDynamicNumOrUndef()) {
                    it->second = Type::number();
                    extraParamNames.push_back(pName);
                }
            }
        }
    }
    if (!i.elseBody.empty()) {
        pushMarker("else", depth + 1);
        scopedStmtList(i.elseBody, depth + 2);
    }
    for (const auto& pName : extraParamNames) {
        if (const auto it = scope_.env.find(pName);
            it != scope_.env.end() && it->second.is(TypeKind::Number)) {
            it->second = Type::dynamicNumOrUndef();
        }
    }
    if (thenTerminates && !elseTerminates) {
        for (const auto& [name, _] : condRefs.whenFalse) {
            if (const auto elseIt = scope_.env.find(name); elseIt != scope_.env.end()) {
                if (const auto thenIt = thenEnv.find(name); thenIt != thenEnv.end()) {
                    thenIt->second = elseIt->second;
                }
            }
        }
    } else if (elseTerminates && !thenTerminates) {
        for (const auto& [name, _] : condRefs.whenTrue) {
            if (const auto thenIt = thenEnv.find(name); thenIt != thenEnv.end()) {
                if (const auto elseIt = scope_.env.find(name); elseIt != scope_.env.end()) {
                    elseIt->second = thenIt->second;
                }
            }
        }
    }
    for (auto it = scope_.hostObjectBindings.begin(); it != scope_.hostObjectBindings.end();) {
        const auto thenIt = hostBindingsThen.find(it->first);
        if (thenIt == hostBindingsThen.end() || thenIt->second != it->second) {
            it = scope_.hostObjectBindings.erase(it);
        } else {
            ++it;
        }
    }
    scope_.env = joinEnv(thenEnv, scope_.env);
    recordMerge(i, scope_.env);
}

Type FlowAnalyzer::ternary(const ast::Ternary& t) {
    expr(*t.condition);
    const Env entry = scope_.env;
    const ConditionRefinements condRefs = extractConditionRefinements(
        t.condition.get(),
        [this](const std::string& name) { return lookup(name); },
        [this](const std::string& name) { return resolvesToUserBinding(name); },
        mod_.mathPristine);
    const auto savedTrue = applyConditionRefinements(
        scope_.env, condRefs.whenTrue,
        [this](const std::string& name) { return scope_.isCell(name); });
    const Type a = expr(*t.thenExpr);
    for (const auto& [name, origT] : savedTrue) {
        if (const auto it = scope_.env.find(name);
            it != scope_.env.end() && condRefs.whenTrue.at(name) == it->second) {
            it->second = origT;
        }
    }
    const Env thenEnv = scope_.env;
    scope_.env = entry;
    const auto savedFalse = applyConditionRefinements(
        scope_.env, condRefs.whenFalse,
        [this](const std::string& name) { return scope_.isCell(name); });
    const Type b = expr(*t.elseExpr);
    for (const auto& [name, origT] : savedFalse) {
        if (const auto it = scope_.env.find(name);
            it != scope_.env.end() && condRefs.whenFalse.at(name) == it->second) {
            it->second = origT;
        }
    }
    scope_.env = joinEnv(thenEnv, scope_.env);
    return join(a, b);
}

void FlowAnalyzer::noteVarInit(const std::string& name, const ast::Expr& init, bool isConst) {
    if (scope_.isCell(name)) return;
    if (isConst) {
        if (const auto* ol = ast::exactAs<const ast::ObjectLit*>(&init);
            ol != nullptr && ol->props.empty()) {
            scope_.dictElemTypes[name] = Type::never();
        }
        if (const auto* maInit = ast::exactAs<const ast::MemberAccess*>(&init);
            maInit != nullptr && !maInit->optional) {
            if (isPristineMathBase(*maInit->object) &&
                isMathMethodReturningNumber(maInit->property)) {
                scope_.pristineMathFns[name] = maInit->property;
            } else if (!maInit->isPrivate && !maInit->property.empty() &&
                       !mod_.methods.isMethodName(maInit->property)) {
                scope_.objectLitMethodAliases[name] = maInit->property;
            }
        }
    }
    if (const auto* callInit = ast::exactAs<const ast::Call*>(&init)) {
        const HostObjectKind kind = classifyHostCallReturnedObject(
            *callInit, [this](const std::string& id) { return resolvesToUserBinding(id); });
        if (kind != HostObjectKind::None) {
            scope_.hostObjectBindings[name] = kind;
        }
    }
}

Type FlowAnalyzer::unary(const ast::Unary& u) {
    const Type operand = exprForOperator(*u.operand);
    const Type result = unaryResult(u.op, operand);
    if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PreDec ||
        u.op == ast::UnaryOp::PostInc || u.op == ast::UnaryOp::PostDec) {
        if (const auto* id = ast::exactAs<const ast::Ident*>(u.operand.get())) {
            assign(id->name, result);
        } else if (const auto* m = ast::exactAs<const ast::MemberAccess*>(u.operand.get())) {
            if (!m->isPrivate) {
                notePropertyWrite(m->property, lastMember_ == m ? lastMemberBase_ : Type::dynamic(),
                                  result);
            }
        }
    }
    return result;
}

Type FlowAnalyzer::binary(const ast::Binary& b) {
    if (b.op == ast::BinaryOp::Assign) {
        const Type rhs = expr(*b.rhs);
        if (const auto* id = ast::exactAs<const ast::Ident*>(b.lhs.get())) {
            assign(id->name, rhs);
            noteVarInit(id->name, *b.rhs, /*isConst=*/false);
        } else {
            expr(*b.lhs);
            if (const auto* m = ast::exactAs<const ast::MemberAccess*>(b.lhs.get())) {
                if (!m->isPrivate) {
                    notePropertyWrite(m->property,
                                      lastMember_ == m ? lastMemberBase_ : Type::dynamic(), rhs);
                    if (!m->optional) {
                        if (const auto* clsId = ast::exactAs<const ast::Ident*>(m->object.get())) {
                            if (mod_.ctors.isClassName(clsId->name)) {
                                Type& slot = mod_.moduleBindings[clsId->name + "." + m->property];
                                slot = join(slot, rhs);
                            }
                        }
                    }
                }
            } else if (const auto* ix = ast::exactAs<const ast::IndexAccess*>(b.lhs.get())) {
                if (const auto* arrId = ast::exactAs<const ast::Ident*>(ix->object.get())) {
                    if (const auto dictIt = scope_.dictElemTypes.find(arrId->name);
                        dictIt != scope_.dictElemTypes.end()) {
                        if (rhs.is(TypeKind::Object) && !rhs.identityOnly() &&
                            rhs.shapeClass() != kNoShapeClass &&
                            mod_.result->classLayouts.byShapeClass(rhs.shapeClass()) == nullptr) {
                            dictIt->second = join(dictIt->second, rhs);
                        } else if (!rhs.is(TypeKind::Never)) {
                            dictIt->second = Type::dynamic();
                        }
                    }
                    if (scope_.env.count(arrId->name) != 0 || scope_.cells.count(arrId->name) != 0) {
                        const Type arrT = lookup(arrId->name);
                        if (arrT.is(TypeKind::Array) &&
                            (arrT.arrayElemClass() != kNoShapeClass || arrT.arrayElementsUndecided())) {
                            Type elemArr = Type::array();
                            if (rhs.is(TypeKind::Never)) {
                                elemArr = Type::arrayOfNever();
                            } else if (rhs.is(TypeKind::Object) && rhs.shapeClass() != kNoShapeClass) {
                                if (mod_.arrayElemShapeClasses.insert(rhs.shapeClass()).second) {
                                    ++mod_.persistentWrites;
                                }
                                elemArr = Type::arrayOfObject(rhs.shapeClass());
                            }
                            const Type updated = join(arrT, elemArr);
                            if (updated != arrT) assign(arrId->name, updated);
                        }
                    }
                }
            }
        }
        return rhs;
    }
    if (ast::isCompoundAssignOp(b.op)) {
        const ast::BinaryOp plain = ast::compoundAssignBase(b.op);
        const Type rhs = exprForOperator(*b.rhs);
        const auto* id = ast::exactAs<const ast::Ident*>(b.lhs.get());
        const Type current = id != nullptr ? lookup(id->name) : exprForOperator(*b.lhs);
        const auto* m = ast::exactAs<const ast::MemberAccess*>(b.lhs.get());
        const Type receiver =
            (m != nullptr && lastMember_ == m) ? lastMemberBase_ : Type::dynamic();
        const Type result = compoundResult(plain, current, rhs);
        if (m != nullptr && !m->isPrivate) {
            notePropertyWrite(m->property, receiver, result);
        }
        if (id != nullptr) assign(id->name, result);
        return result;
    }
    if (b.op == ast::BinaryOp::LogicalAnd || b.op == ast::BinaryOp::LogicalOr ||
        b.op == ast::BinaryOp::NullishCoalescing) {
        const Type l = expr(*b.lhs);
        const Env entry = scope_.env;
        const Type r = expr(*b.rhs);
        scope_.env = joinEnv(entry, scope_.env);
        if (b.op == ast::BinaryOp::NullishCoalescing) return nullishCoalescingResult(l, r);
        if (b.op == ast::BinaryOp::LogicalOr) return logicalOrResult(l, r);
        return join(l, r);
    }
    if (b.op == ast::BinaryOp::Comma) {
        const Type l = expr(*b.lhs);
        const Type r = expr(*b.rhs);
        return binaryResult(b.op, l, r);
    }
    const Type l = exprForOperator(*b.lhs);
    const Type r = exprForOperator(*b.rhs);
    return binaryResult(b.op, l, r);
}

}  // namespace bronze::types
