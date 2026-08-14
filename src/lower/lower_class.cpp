// Classes, desugared into what prototypes already provide: a constructor
// function, a prototype object holding the methods, and — for `extends` — the
// two prototype links that make inheritance work. A class introduces no runtime
// concept of its own, so there is nothing here that a program could not have
// written by hand with `function` and `.prototype`; what it buys is that bronze
// can now read the source three.js is actually written in.

#include <string>
#include <vector>

#include "ast/clone.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// Reads `<ctor>.prototype`, the object every instance method is stored on.
Lowerer::Value Lowerer::emitPrototypeOf(Value ctorVal, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {ctorVal.id};
    inst.keyIndex = getKeyConstantIndex("prototype");
    inst.icIndex = icSiteCounter_++;
    inst.icMonomorphic = false;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerClass(const std::string& name,
                                                  const std::string& superName,
                                                  const std::vector<ast::ClassMethod>& methods,
                                                  Span span, il::Function& ilFn) {
    const ast::ClassMethod* ctor = nullptr;
    for (const auto& m : methods) {
        if (m.isConstructor) {
            ctor = &m;
            break;
        }
    }
    if (!ctor) {
        // The parser synthesizes one for a class that does not write it, so
        // reaching here is a drift between the two, not a program error.
        diags_.error(span, "internal: class with no constructor method");
        return std::nullopt;
    }

    // The heritage is READ before the class binding is initialized. 15.7.14
    // evaluates ClassHeritage at step 5 and initializes `classBinding` only at
    // step 17, which is what makes `class C extends C {}` a ReferenceError
    // rather than a class extending itself: inside its own definition the name
    // is still in its dead zone.
    std::optional<Value> baseBoxed;
    if (!superName.empty()) {
        ast::Ident baseIdent;
        baseIdent.name = superName;
        baseIdent.span = span;
        auto baseVal = lowerExpr(baseIdent, ilFn);
        if (!baseVal) return std::nullopt;
        baseBoxed = boxValueIfNeeded(*baseVal, ilFn);
    }

    // Instance fields injection:
    // Build the constructor body with instance fields injected.
    std::vector<ast::StmtPtr> fieldStmts;
    for (const auto& m : methods) {
        if (m.isField && !m.isStatic) {
            auto thisExpr = std::make_unique<ast::ThisExpr>();
            thisExpr->span = m.fn ? m.fn->span : span;

            ast::ExprPtr target;
            if (m.keyExpr) {
                auto idx = std::make_unique<ast::IndexAccess>();
                idx->span = thisExpr->span;
                idx->object = std::move(thisExpr);
                idx->index = ast::cloneExpr(*m.keyExpr);
                target = std::move(idx);
            } else {
                auto mem = std::make_unique<ast::MemberAccess>();
                mem->span = thisExpr->span;
                mem->object = std::move(thisExpr);
                mem->property = m.name;
                target = std::move(mem);
            }

            auto bin = std::make_unique<ast::Binary>();
            bin->span = target->span;
            bin->op = ast::BinaryOp::Assign;
            bin->lhs = std::move(target);
            if (m.init) {
                bin->rhs = ast::cloneExpr(*m.init);
            } else {
                auto undef = std::make_unique<ast::UndefinedLit>();
                undef->span = bin->span;
                bin->rhs = std::move(undef);
            }

            auto stmt = std::make_unique<ast::ExprStmt>();
            stmt->span = bin->span;
            stmt->expr = std::move(bin);
            fieldStmts.push_back(std::move(stmt));
        }
    }

    std::vector<ast::StmtPtr> ctorBody;
    if (fieldStmts.empty()) {
        for (const auto& s : ctor->fn->body) ctorBody.push_back(ast::cloneStmt(*s));
    } else if (superName.empty()) {
        // Base class: fields run at the start of constructor
        for (auto& fs : fieldStmts) ctorBody.push_back(std::move(fs));
        for (const auto& s : ctor->fn->body) ctorBody.push_back(ast::cloneStmt(*s));
    } else {
        // Derived class: fields run after super(...)
        bool inserted = false;
        for (const auto& s : ctor->fn->body) {
            ctorBody.push_back(ast::cloneStmt(*s));
            if (!inserted) {
                if (const auto* es = dynamic_cast<const ast::ExprStmt*>(s.get())) {
                    if (dynamic_cast<const ast::SuperCall*>(es->expr.get())) {
                        for (auto& fs : fieldStmts) ctorBody.push_back(std::move(fs));
                        inserted = true;
                    }
                }
            }
        }
        if (!inserted) {
            for (size_t i = 0; i < fieldStmts.size(); ++i) {
                ctorBody.insert(ctorBody.begin() + i, std::move(fieldStmts[i]));
            }
        }
    }

    // The class IS its constructor function, and the binding the declaration
    // introduces holds exactly that value.
    // 15.7.14 step 15: the constructor's `name` is the CLASS's name, not the
    // parser's synthesized `constructor`, and its `length` is the constructor's
    // own parameter list.
    auto ctorVal = lowerClosure(*ctor->fn, name, name, ctor->fn->params,
                                ctor->fn->returnType, ctorBody, span, ilFn);
    if (!ctorVal) return std::nullopt;

    // `extends` REPLACES the prototype object (the prototype lives on the
    // shape), so it has to be linked before a single method is stored.
    if (baseBoxed) {
        il::Instruction inst;
        inst.op = il::Op::ClassExtend;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {ctorVal->id, baseBoxed->id};
        emitInst(ilFn, inst);
    }

    bool needsPrototype = false;
    for (const auto& m : methods) {
        if (!m.isConstructor && !m.isStatic && !m.isField) needsPrototype = true;
    }
    Value protoVal{il::kNoValue, il::Type::Dynamic};
    if (needsPrototype) protoVal = emitPrototypeOf(*ctorVal, ilFn);

    for (const auto& m : methods) {
        if (m.isConstructor || m.isField) continue;
        const il::ValueId homeObject = m.isStatic ? ctorVal->id : protoVal.id;

        // A class accessor is NON-enumerable — ECMA-262 15.7.14 defines it
        // exactly as it defines a method, and the two therefore share the rule
        // that keeps them out of `for-in`.
        if (m.accessor != ast::AccessorKind::None) {
            if (m.keyExpr) {
                auto keyOpt = lowerExpr(*m.keyExpr, ilFn);
                if (!keyOpt) return std::nullopt;
                auto keyBoxed = boxValueIfNeeded(*keyOpt, ilFn);
                if (!emitAccessorDefComputed(Value{homeObject, il::Type::Dynamic}, keyBoxed,
                                             m.accessor, *m.fn, /*enumerable=*/false, ilFn)) {
                    return std::nullopt;
                }
            } else {
                if (!emitAccessorDef(Value{homeObject, il::Type::Dynamic}, m.name, m.accessor, *m.fn,
                                     /*enumerable=*/false, ilFn)) {
                    return std::nullopt;
                }
            }
            continue;
        }

        // A COMPUTED member name is an expression evaluated where the class is
        // defined, and BEFORE the method it names (15.7.14 evaluates the
        // ClassElementName first).
        std::optional<Value> keyBoxed;
        if (m.keyExpr) {
            auto keyOpt = lowerExpr(*m.keyExpr, ilFn);
            if (!keyOpt) return std::nullopt;
            keyBoxed = boxValueIfNeeded(*keyOpt, ilFn);
        }

        auto fnVal = lowerClosure(
            *m.fn, m.fn->name,
            m.keyExpr ? std::optional<std::string>{} : std::optional<std::string>{m.name},
            m.fn->params, m.fn->returnType, m.fn->body, m.fn->span, ilFn);
        if (!fnVal) return std::nullopt;

        if (keyBoxed) {
            il::Instruction computedInst;
            computedInst.op = il::Op::MethodDefComputed;
            computedInst.type = il::Type::Void;
            computedInst.result = il::kNoValue;
            computedInst.operands = {homeObject, keyBoxed->id, fnVal->id};
            emitInst(ilFn, computedInst);
            continue;
        }

        il::Instruction setInst;
        setInst.op = il::Op::MethodDef;
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.operands = {homeObject, fnVal->id};
        setInst.keyIndex = getKeyConstantIndex(m.name);
        emitInst(ilFn, setInst);
    }

    // Static fields evaluation:
    for (const auto& m : methods) {
        if (!m.isField || !m.isStatic) continue;
        if (m.keyExpr) {
            auto keyOpt = lowerExpr(*m.keyExpr, ilFn);
            if (!keyOpt) return std::nullopt;
            auto keyBoxed = boxValueIfNeeded(*keyOpt, ilFn);
            std::optional<Value> initVal;
            if (m.init) {
                initVal = lowerExpr(*m.init, ilFn);
            } else {
                initVal = Value{emitConstUndefined(ilFn), il::Type::Dynamic};
            }
            if (!initVal) return std::nullopt;
            auto initBoxed = boxValueIfNeeded(*initVal, ilFn);
            il::Instruction setInst;
            setInst.op = il::Op::ElemSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {ctorVal->id, keyBoxed.id, initBoxed.id};
            setInst.immI32 = 0;
            emitInst(ilFn, setInst);
        } else {
            std::optional<Value> initVal;
            if (m.init) {
                initVal = lowerExpr(*m.init, ilFn);
            } else {
                initVal = Value{emitConstUndefined(ilFn), il::Type::Dynamic};
            }
            if (!initVal) return std::nullopt;
            auto initBoxed = boxValueIfNeeded(*initVal, ilFn);
            il::Instruction setInst;
            setInst.op = il::Op::PropSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {ctorVal->id, initBoxed.id};
            setInst.keyIndex = getKeyConstantIndex(m.name);
            setInst.icIndex = icSiteCounter_++;
            emitInst(ilFn, setInst);
        }
    }

    return ctorVal;
}

bool Lowerer::lowerClassDecl(const ast::ClassDecl* cls, il::Function& ilFn) {
    auto ctorVal = lowerClass(cls->name, cls->superName, cls->methods, cls->span, ilFn);
    if (!ctorVal) return false;
    if (!declareVariable(cls->name, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/true,
                         /*isVar=*/false, /*isInitialized=*/true, ctorVal->id, cls->span)) {
        return false;
    }
    VarBinding& bound = varBindings_[activeVarMap_[cls->name]];
    if (bound.inEnv) {
        emitEnvSet(envDepthOf(bound.envScopeIndex), bound.envSlot, *ctorVal, ilFn);
    }
    return true;
}

std::optional<Lowerer::Value> Lowerer::lowerClassExpr(const ast::ClassExpr* cls, il::Function& ilFn) {
    return lowerClass(cls->name, cls->superName, cls->methods, cls->span, ilFn);
}

// `super.m` — the lookup starts at the PARENT prototype, which is why it cannot
// be written as `this.m`: inside an override, `this.m` would find the override
// again and recurse forever. The parent is named at the site, so this is two
// ordinary property reads.
std::optional<Lowerer::Value> Lowerer::lowerSuperMember(const ast::SuperMember* sm,
                                                        il::Function& ilFn) {
    ast::Ident baseIdent;
    baseIdent.name = sm->baseName;
    baseIdent.span = sm->span;
    auto baseVal = lowerExpr(baseIdent, ilFn);
    if (!baseVal) return std::nullopt;
    auto protoVal = emitPrototypeOf(boxValueIfNeeded(*baseVal, ilFn), ilFn);

    // The receiver is `this`, not the prototype the lookup starts from
    // (13.3.7.3). Indistinguishable from an ordinary read for a method and not
    // at all for an accessor, which is why this stopped being a plain
    // `prop.get` when accessors landed.
    auto thisVal = lowerThisValue(sm->span, ilFn);
    if (!thisVal) return std::nullopt;

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::SuperGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {protoVal.id, boxValueIfNeeded(*thisVal, ilFn).id};
    inst.keyIndex = getKeyConstantIndex(sm->property);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

// `super(...)` — the parent constructor, run on the receiver this
// constructor is already building. Not a `new`: there is one object, and the
// parent is only being given its chance to initialize it.
std::optional<Lowerer::Value> Lowerer::lowerSuperCall(const ast::SuperCall* sc,
                                                      il::Function& ilFn) {
    auto thisVal = lowerThisValue(sc->span, ilFn);
    if (!thisVal) return std::nullopt;

    ast::Ident baseIdent;
    baseIdent.name = sc->baseName;
    baseIdent.span = sc->span;
    auto baseVal = lowerExpr(baseIdent, ilFn);
    if (!baseVal) return std::nullopt;

    // `super(...args)` is how a DERIVED CLASS WITH NO CONSTRUCTOR forwards, so
    // this path is not an edge case: it is the default one.
    const bool spreadArgs = listHasSpread(sc->args);
    std::vector<il::ValueId> operands;
    operands.push_back(boxValueIfNeeded(*baseVal, ilFn).id);
    operands.push_back(boxValueIfNeeded(*thisVal, ilFn).id);
    if (spreadArgs) {
        auto argsArr = lowerListToArray(sc->args, ilFn);
        if (!argsArr) return std::nullopt;
        operands.push_back(argsArr->id);
    } else {
        for (const auto& argPtr : sc->args) {
            auto argVal = lowerExpr(*argPtr, ilFn);
            if (!argVal) return std::nullopt;
            operands.push_back(boxValueIfNeeded(*argVal, ilFn).id);
        }
    }

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = spreadArgs ? il::Op::SuperCallSpread : il::Op::SuperCall;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(operands);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
