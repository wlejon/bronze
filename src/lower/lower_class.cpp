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
                                                  const ast::Expr* superClass,
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

    const bool hasSuper = superClass != nullptr || !superName.empty();

    // The private names this class declares, and the record that holds them.
    // Opened BEFORE the heritage is read, which is 15.7.14's order (the class's
    // private environment is created at step 2 and the heritage evaluated at
    // step 5) and also what the constructor needs: its closure is created below
    // and must capture this record.
    const std::vector<PrivateElement> privateElements = collectPrivateElements(methods);
    bool hasStaticBlock = false;
    for (const auto& m : methods) hasStaticBlock = hasStaticBlock || m.isStaticBlock;
    // A class with neither private names nor a static block needs no record at
    // all, and gets exactly the IL it always had.
    const bool hasPrivate = !privateElements.empty() || hasStaticBlock;
    if (hasPrivate && !openClassScope(name, privateElements, ilFn)) return std::nullopt;
    // Everything from here on may leave through a `return std::nullopt`, and
    // the record must come off both stacks when it does.
    struct PrivateScopeGuard {
        Lowerer* self;
        bool active;
        ~PrivateScopeGuard() {
            if (!active) return;
            self->privateScopes_.pop_back();
            self->exitScope();
        }
    } privateGuard{this, hasPrivate};

    // The heritage is READ before the class binding is initialized. 15.7.14
    // evaluates ClassHeritage at step 5 and initializes `classBinding` only at
    // step 17, which is what makes `class C extends C {}` a ReferenceError
    // rather than a class extending itself: inside its own definition the name
    // is still in its dead zone.
    std::optional<Value> baseBoxed;
    if (superClass) {
        auto baseVal = lowerExpr(*superClass, ilFn);
        if (!baseVal) return std::nullopt;
        baseBoxed = boxValueIfNeeded(*baseVal, ilFn);
    } else if (!superName.empty()) {
        ast::Ident baseIdent;
        baseIdent.name = superName;
        baseIdent.span = span;
        auto baseVal = lowerExpr(baseIdent, ilFn);
        if (!baseVal) return std::nullopt;
        baseBoxed = boxValueIfNeeded(*baseVal, ilFn);
    }

    // The constructor's prologue: brand adds for the private methods and
    // accessors, then every field initializer — public and private alike — in
    // definition order (lower_private.cpp says why the order is observable).
    std::vector<ast::StmtPtr> fieldStmts =
        buildFieldInitStatements(methods, privateElements, span);

    std::vector<ast::StmtPtr> ctorBody;
    if (fieldStmts.empty()) {
        for (const auto& s : ctor->fn->body) ctorBody.push_back(ast::cloneStmt(*s));
    } else if (!hasSuper) {
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

    // A PRIVATE method is not stored on the prototype and a static block is
    // not stored anywhere, so neither makes a class need one read.
    bool needsPrototype = false;
    for (const auto& m : methods) {
        if (!m.isConstructor && !m.isStatic && !m.isField && !m.isStaticBlock && !m.isPrivate()) {
            needsPrototype = true;
        }
    }
    Value protoVal{il::kNoValue, il::Type::Dynamic};
    if (needsPrototype) protoVal = emitPrototypeOf(*ctorVal, ilFn);

    for (const auto& m : methods) {
        if (m.isConstructor || m.isField || m.isStaticBlock) continue;
        // A private method or accessor is SHARED — one closure per class
        // evaluation, not one per instance — so it is created here, once, and
        // parked in the class-evaluation record. What each object gets is the
        // brand: an entry in the name's table pointing at this same closure,
        // which is why `a.#m === b.#m` for two instances of one evaluation.
        // It is deliberately NOT on the prototype: a private name is not a
        // property key, and a prototype slot would be reachable.
        if (m.isPrivate()) {
            auto fnVal = lowerClosure(*m.fn, m.fn->name, std::optional<std::string>{m.name},
                                      m.fn->params, m.fn->returnType, m.fn->body, m.fn->span, ilFn);
            if (!fnVal) return std::nullopt;
            const std::string slot = m.accessor == ast::AccessorKind::Setter
                                         ? privateSetterFnSlot(m.name)
                                         : privateFnSlot(m.name);
            uint32_t depth = 0;
            uint32_t index = 0;
            if (!findEnclosingEnvVar(slot, depth, index)) {
                diags_.error(m.fn->span, "internal: no environment slot for private member '" +
                                             m.name + "'");
                return std::nullopt;
            }
            emitEnvSet(depth, index, *fnVal, ilFn);
            continue;
        }
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

    // 15.7.14 step 28: the class binding is initialized once every method is
    // defined and before any static element is evaluated, which is what makes
    // `static { C.#x = 1 }` reach the class by name.
    if (hasPrivate && !name.empty() && !initClassNameBinding(name, *ctorVal, span, ilFn)) {
        return std::nullopt;
    }

    // Static private methods and accessors are carried by the CONSTRUCTOR, and
    // their brand is added before any static initializer runs — 15.7.14 does it
    // at step 32, ahead of the element evaluation below, so a static block may
    // call `X.#m()`.
    if (hasPrivate && !emitStaticPrivateBrands(privateElements, *ctorVal, span, ilFn)) {
        return std::nullopt;
    }

    // Static elements, in DEFINITION order: 15.7.14 step 33 walks the element
    // list once, so a static block and a static field initializer written
    // around it run in the order the source wrote them and not in two passes.
    for (const auto& m : methods) {
        // `static { ... }` — a body evaluated once with the constructor as its
        // receiver. It is a closure and a call, because that is exactly what it
        // is: 15.7.11 makes the block a function whose [[HomeObject]] is the
        // class and whose `this` is the constructor.
        if (m.isStaticBlock) {
            auto blockFn = lowerClosure(*m.fn, m.fn->name, std::optional<std::string>{""},
                                        m.fn->params, m.fn->returnType, m.fn->body, m.fn->span,
                                        ilFn);
            if (!blockFn) return std::nullopt;
            emitPrivateCall(boxValueIfNeeded(*blockFn, ilFn), *ctorVal, {}, ilFn);
            continue;
        }
        if (!m.isField || !m.isStatic) continue;
        // A static PRIVATE field: the constructor is the object that carries
        // it, so this is the same definition an instance field makes, against
        // the class itself.
        if (m.isPrivate()) {
            std::optional<Value> initVal =
                m.init ? lowerExpr(*m.init, ilFn)
                       : std::optional<Value>{Value{emitConstUndefined(ilFn), il::Type::Dynamic}};
            if (!initVal) return std::nullopt;
            auto table = emitPrivateSlotRead(privateTableSlot(m.name), span, ilFn);
            if (!table) return std::nullopt;
            il::Instruction addInst;
            addInst.op = il::Op::PrivateAdd;
            addInst.type = il::Type::Void;
            addInst.result = il::kNoValue;
            addInst.operands = {table->id, ctorVal->id, boxValueIfNeeded(*initVal, ilFn).id};
            emitInst(ilFn, addInst);
            continue;
        }
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
    auto ctorVal = lowerClass(cls->name, cls->superClass.get(), cls->superName, cls->methods, cls->span, ilFn);
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
    return lowerClass(cls->name, cls->superClass.get(), cls->superName, cls->methods, cls->span, ilFn);
}

// `super.m` — the lookup starts at the PARENT prototype, which is why it cannot
// be written as `this.m`: inside an override, `this.m` would find the override
// again and recurse forever. The parent is named at the site, so this is two
// ordinary property reads.
std::optional<Lowerer::Value> Lowerer::lowerSuperMember(const ast::SuperMember* sm,
                                                        il::Function& ilFn) {
    std::optional<Value> baseVal;
    if (sm->baseExpr) {
        baseVal = lowerExpr(*sm->baseExpr, ilFn);
    } else {
        ast::Ident baseIdent;
        baseIdent.name = sm->baseName;
        baseIdent.span = sm->span;
        baseVal = lowerExpr(baseIdent, ilFn);
    }
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

    std::optional<Value> baseVal;
    if (sc->baseExpr) {
        baseVal = lowerExpr(*sc->baseExpr, ilFn);
    } else {
        ast::Ident baseIdent;
        baseIdent.name = sc->baseName;
        baseIdent.span = sc->span;
        baseVal = lowerExpr(baseIdent, ilFn);
    }
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
