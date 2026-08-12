// Classes, desugared into what prototypes already provide: a constructor
// function, a prototype object holding the methods, and — for `extends` — the
// two prototype links that make inheritance work. A class introduces no runtime
// concept of its own, so there is nothing here that a program could not have
// written by hand with `function` and `.prototype`; what it buys is that bronze
// can now read the source three.js is actually written in.

#include <string>
#include <vector>

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

bool Lowerer::lowerClassDecl(const ast::ClassDecl* cls, il::Function& ilFn) {
    const ast::ClassMethod* ctor = nullptr;
    for (const auto& m : cls->methods) {
        if (m.isConstructor) {
            ctor = &m;
            break;
        }
    }
    if (!ctor) {
        // The parser synthesizes one for a class that does not write it, so
        // reaching here is a drift between the two, not a program error.
        diags_.error(cls->span, "internal: class with no constructor method");
        return false;
    }

    // The class IS its constructor function, and the binding the declaration
    // introduces holds exactly that value.
    auto ctorVal = lowerClosure(*ctor->fn, cls->name, ctor->fn->params, ctor->fn->returnType,
                                ctor->fn->body, cls->span, ilFn);
    if (!ctorVal) return false;
    if (!declareVariable(cls->name, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/true,
                         /*isVar=*/false, /*isInitialized=*/true, ctorVal->id, cls->span)) {
        return false;
    }
    VarBinding& bound = varBindings_[activeVarMap_[cls->name]];
    if (bound.inEnv) {
        emitEnvSet(envDepthOf(bound.envScopeIndex), bound.envSlot, *ctorVal, ilFn);
    }

    // `extends` REPLACES the prototype object (the prototype lives on the
    // shape), so it has to run before a single method is stored.
    if (!cls->superName.empty()) {
        ast::Ident baseIdent;
        baseIdent.name = cls->superName;
        baseIdent.span = cls->span;
        auto baseVal = lowerExpr(baseIdent, ilFn);
        if (!baseVal) return false;
        auto baseBoxed = boxValueIfNeeded(*baseVal, ilFn);

        il::Instruction inst;
        inst.op = il::Op::ClassExtend;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {ctorVal->id, baseBoxed.id};
        emitInst(ilFn, inst);
    }

    bool needsPrototype = false;
    for (const auto& m : cls->methods) {
        if (!m.isConstructor && !m.isStatic) needsPrototype = true;
    }
    Value protoVal{il::kNoValue, il::Type::Dynamic};
    if (needsPrototype) protoVal = emitPrototypeOf(*ctorVal, ilFn);

    for (const auto& m : cls->methods) {
        if (m.isConstructor) continue;
        const il::ValueId homeObject = m.isStatic ? ctorVal->id : protoVal.id;

        // A class accessor is NON-enumerable — ECMA-262 15.7.14 defines it
        // exactly as it defines a method, and the two therefore share the rule
        // that keeps them out of `for-in`.
        if (m.accessor != ast::AccessorKind::None) {
            if (!emitAccessorDef(Value{homeObject, il::Type::Dynamic}, m.name, m.accessor, *m.fn,
                                 /*enumerable=*/false, ilFn)) {
                return false;
            }
            continue;
        }

        auto fnVal = lowerClosure(*m.fn, m.fn->name, m.fn->params, m.fn->returnType, m.fn->body,
                                  m.fn->span, ilFn);
        if (!fnVal) return false;

        // An instance method belongs to the prototype, shared by every
        // instance; a `static` one belongs to the constructor itself, which is
        // an own property of the function object.
        //
        // `method.def` rather than `prop.set`, because ECMA-262 15.7.14 defines
        // a method with `enumerable: false` and an assignment cannot say that.
        // It is what keeps a method out of `Object.keys`, out of an object
        // spread, and out of `for-in` — where it would otherwise show up on
        // every instance of the class.
        il::Instruction setInst;
        setInst.op = il::Op::MethodDef;
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.operands = {homeObject, fnVal->id};
        setInst.keyIndex = getKeyConstantIndex(m.name);
        emitInst(ilFn, setInst);
    }
    return true;
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
    inst.op = spreadArgs ? il::Op::DynamicCallSpread : il::Op::DynamicCall;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(operands);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
