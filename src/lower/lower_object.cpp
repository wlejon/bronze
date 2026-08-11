// Object and array literals, property and element access, `new`, and the
// call forms — the expressions that go through the runtime's shapes,
// prototypes and inline caches (docs/0008).

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerObjectLit(const ast::ObjectLit* objLit,
                                                      il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CreateObject;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    emitInst(ilFn, inst);

    for (const auto& prop : objLit->props) {
        auto valOpt = lowerExpr(*prop.value, ilFn);
        if (!valOpt) return std::nullopt;
        auto valBoxed = boxValueIfNeeded(*valOpt, ilFn);

        uint32_t keyIdx = getKeyConstantIndex(prop.key);
        uint32_t icIdx = icSiteCounter_++;

        il::Instruction setInst;
        setInst.op = il::Op::PropSet;
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.operands = {res, valBoxed.id};
        setInst.keyIndex = keyIdx;
        setInst.icIndex = icIdx;
        emitInst(ilFn, setInst);
    }
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerArrayLit(const ast::ArrayLit* arrLit,
                                                     il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CreateArray;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.immI32 = static_cast<int32_t>(arrLit->elements.size());
    emitInst(ilFn, inst);

    for (size_t i = 0; i < arrLit->elements.size(); ++i) {
        auto elemOpt = lowerExpr(*arrLit->elements[i], ilFn);
        if (!elemOpt) return std::nullopt;
        auto elemBoxed = boxValueIfNeeded(*elemOpt, ilFn);

        uint32_t keyIdx = getKeyConstantIndex(std::to_string(i));
        uint32_t icIdx = icSiteCounter_++;

        il::Instruction setInst;
        setInst.op = il::Op::PropSet;
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.operands = {res, elemBoxed.id};
        setInst.keyIndex = keyIdx;
        setInst.icIndex = icIdx;
        emitInst(ilFn, setInst);
    }
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerNewExpr(const ast::NewExpr* newExpr,
                                                    il::Function& ilFn) {
    if (newExpr->callee == "Float32Array" || newExpr->callee == "ArrayBuffer") {
        if (newExpr->args.size() != 1) {
            diags_.error(newExpr->span, "unsupported construct: new " + newExpr->callee +
                                            " expects exactly one argument");
            return std::nullopt;
        }
        auto argVal = lowerExpr(*newExpr->args[0], ilFn);
        if (!argVal) return std::nullopt;
        auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = (newExpr->callee == "Float32Array") ? il::Op::CreateFloat32Array
                                                      : il::Op::CreateArrayBuffer;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {argBoxed.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }
    // Any other callee is an ordinary value: the whole ceremony
    // (prototype, instance shape, receiver, result rule) lives in
    // one runtime helper rather than in codegen — docs/0008
    // decision 4.
    ast::Ident calleeIdent;
    calleeIdent.name = newExpr->callee;
    calleeIdent.span = newExpr->span;
    auto calleeVal = lowerExpr(calleeIdent, ilFn);
    if (!calleeVal) return std::nullopt;

    std::vector<il::ValueId> operands;
    operands.push_back(boxValueIfNeeded(*calleeVal, ilFn).id);
    for (const auto& argPtr : newExpr->args) {
        auto argVal = lowerExpr(*argPtr, ilFn);
        if (!argVal) return std::nullopt;
        operands.push_back(boxValueIfNeeded(*argVal, ilFn).id);
    }

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::Construct;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(operands);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerMemberAccess(const ast::MemberAccess* mem,
                                                         il::Function& ilFn) {
    auto objVal = lowerExpr(*mem->object, ilFn);
    if (!objVal) return std::nullopt;
    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

    uint32_t keyIdx = getKeyConstantIndex(mem->property);
    uint32_t icIdx = icSiteCounter_++;

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {objBoxed.id};
    inst.keyIndex = keyIdx;
    inst.icIndex = icIdx;
    inst.icMonomorphic = monomorphicPropSite(*mem->object);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

// `o["k"]` and `o[2]` are property reads whose key is known here, so they fold
// onto the same PropGet — and the same inline cache — that `o.k` uses.
//
// A NUMERIC literal folds only when it is a canonical array index. A property
// key is ToString(Number), and `std::to_string` of an integer is that string
// only for a non-negative integer in the array-index range: `a[1.5]` names the
// property "1.5", so folding it to "1" would read element 1 (`index_keys`).
// Everything else takes the elem path, where the index stays a value and the
// runtime applies the language's own rule to it.
std::optional<uint32_t> Lowerer::literalIndexKey(const ast::Expr& index) {
    if (const auto* strLit = dynamic_cast<const ast::StringLit*>(&index)) {
        return getKeyConstantIndex(strLit->value);
    }
    const auto* numLit = dynamic_cast<const ast::NumberLit*>(&index);
    if (numLit == nullptr) return std::nullopt;
    const double v = numLit->value;
    if (!(v >= 0.0) || v != std::floor(v) || v > 4294967294.0) return std::nullopt;
    return getKeyConstantIndex(std::to_string(static_cast<uint64_t>(v)));
}

std::optional<Lowerer::Value> Lowerer::lowerIndexAccess(const ast::IndexAccess* idxAccess,
                                                        il::Function& ilFn) {
    auto objVal = lowerExpr(*idxAccess->object, ilFn);
    if (!objVal) return std::nullopt;
    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);

    const std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess->index);
    if (!literalKey) {
        // Computed index: a real elem.get on the index value.
        auto indexVal = lowerExpr(*idxAccess->index, ilFn);
        if (!indexVal) return std::nullopt;
        auto idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ElemGet;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {objBoxed.id, idxBoxed.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    uint32_t icIdx = icSiteCounter_++;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {objBoxed.id};
    inst.keyIndex = *literalKey;
    inst.icIndex = icIdx;
    inst.icMonomorphic = monomorphicPropSite(*idxAccess->object);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerCall(const ast::Call* call, il::Function& ilFn) {
    bool isConsoleLog = false;
    if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
        if (calleeIdent->name == "console.log") isConsoleLog = true;
    } else if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        if (const auto* baseIdent = dynamic_cast<const ast::Ident*>(mem->object.get())) {
            if (baseIdent->name == "console" && mem->property == "log") isConsoleLog = true;
        }
    }

    if (isConsoleLog) {
        if (call->args.size() != 1) {
            diags_.error(call->span, "console.log expects 1 argument");
            return std::nullopt;
        }
        auto argVal = lowerExpr(*call->args[0], ilFn);
        if (!argVal) return std::nullopt;

        auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
        il::Instruction inst;
        inst.op = il::Op::Print;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {argBoxed.id};
        emitInst(ilFn, inst);
        return Value{il::kNoValue, il::Type::Void};
    }

    // `Object` is recognized here rather than looked up: bronze has
    // no global object for it to live on (docs/0009 decision 2).
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        const auto* baseIdent = dynamic_cast<const ast::Ident*>(mem->object.get());
        if (baseIdent && baseIdent->name == "Object" &&
            activeVarMap_.find("Object") == activeVarMap_.end()) {
            if (mem->property != "keys") {
                diags_.error(call->span, "unsupported builtin: Object." + mem->property);
                return std::nullopt;
            }
            if (call->args.size() != 1) {
                diags_.error(call->span, "Object.keys expects 1 argument");
                return std::nullopt;
            }
            auto argVal = lowerExpr(*call->args[0], ilFn);
            if (!argVal) return std::nullopt;
            auto argBoxed = boxValueIfNeeded(*argVal, ilFn);

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ObjectKeys;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {argBoxed.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }
    }

    if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
        // A local binding shadows a module-level function, and so
        // does an enclosing scope's environment slot: a nested
        // function declaration registers in functionIndices_ under
        // its source name, but every reference to it — including a
        // sibling closure's — has to go through the environment
        // chain, not a direct call (docs/0007 decision 3).
        uint32_t shadowDepth = 0;
        uint32_t shadowIndex = 0;
        auto envIt = activeVarMap_.find(calleeIdent->name);
        if (envIt == activeVarMap_.end() &&
            !findEnclosingEnvVar(calleeIdent->name, shadowDepth, shadowIndex)) {
            auto it = functionIndices_.find(calleeIdent->name);
            if (it != functionIndices_.end()) {
                uint32_t calleeIdx = it->second;
                const auto& calleeFn = ilModule_.functions[calleeIdx];

                // Synthetic parameters are not source arguments;
                // the arity the program has to match is the source
                // one.
                const size_t base = calleeFn.firstSourceParam();
                if (call->args.size() != calleeFn.params.size() - base) {
                    diags_.error(call->span, "argument count mismatch in call to " + calleeIdent->name);
                    return std::nullopt;
                }

                std::vector<il::ValueId> argVals;
                // A plain `f()` has no receiver, so a direct call
                // supplies undefined for `__this` (docs/0008
                // decision 3). `__env` cannot be supplied this way,
                // which is why the verifier forbids direct calls to
                // closures outright.
                if (calleeFn.needsThis) {
                    argVals.push_back(emitConstUndefined(ilFn));
                }
                for (size_t i = 0; i < call->args.size(); ++i) {
                    auto argVal = lowerExpr(*call->args[i], ilFn);
                    if (!argVal) return std::nullopt;
                    const il::Type paramType = calleeFn.params[i + base].type;
                    if (paramType == il::Type::Dynamic) {
                        argVal = boxValueIfNeeded(*argVal, ilFn);
                    } else if (argVal->type == il::Type::Dynamic) {
                        argVal = unboxValueIfNeeded(*argVal, paramType, ilFn);
                    }
                    argVals.push_back(argVal->id);
                }

                il::Instruction inst;
                inst.op = il::Op::Call;
                inst.calleeIndex = calleeIdx;
                inst.operands = std::move(argVals);
                inst.type = calleeFn.returnType;

                il::ValueId res = il::kNoValue;
                if (calleeFn.returnType != il::Type::Void) {
                    res = ilFn.valueCount++;
                    inst.result = res;
                } else {
                    inst.result = il::kNoValue;
                }

                emitInst(ilFn, inst);
                if (calleeFn.returnType == il::Type::Void) {
                    // A function that returns no value still EVALUATES to
                    // one: `undefined`. The IL return type stays void so the
                    // call itself costs nothing, and the undefined is
                    // materialized here, where the only thing that can see it
                    // is the expression the call sits in. Handing back the
                    // void call's absent result instead let it escape into
                    // arbitrary expression contexts — `console.log(f())`
                    // reached the verifier as a box of value %kNoValue and
                    // failed to compile at all.
                    return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
                }
                return Value{res, calleeFn.returnType};
            }
        }
    }

    Value calleeVal;
    Value thisArgVal;
    // `super.m(...)` is the one call whose callee and receiver come from
    // different objects: the function is found on the PARENT prototype, and
    // it runs on the current receiver (docs/0012 decision 5).
    if (const auto* superMem = dynamic_cast<const ast::SuperMember*>(call->callee.get())) {
        auto fnVal = lowerSuperMember(superMem, ilFn);
        if (!fnVal) return std::nullopt;
        auto thisVal = lowerThisValue(call->span, ilFn);
        if (!thisVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*fnVal, ilFn);
        thisArgVal = boxValueIfNeeded(*thisVal, ilFn);
    } else if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        auto objVal = lowerExpr(*mem->object, ilFn);
        if (!objVal) return std::nullopt;
        thisArgVal = boxValueIfNeeded(*objVal, ilFn);

        uint32_t keyIdx = getKeyConstantIndex(mem->property);
        uint32_t icIdx = icSiteCounter_++;

        il::ValueId getRes = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::PropGet;
        inst.type = il::Type::Dynamic;
        inst.result = getRes;
        inst.operands = {thisArgVal.id};
        inst.keyIndex = keyIdx;
        inst.icIndex = icIdx;
        inst.icMonomorphic = monomorphicPropSite(*mem->object);
        emitInst(ilFn, inst);
        calleeVal = Value{getRes, il::Type::Dynamic};
    } else {
        auto cVal = lowerExpr(*call->callee, ilFn);
        if (!cVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*cVal, ilFn);

        il::ValueId zeroRes = ilFn.valueCount++;
        il::Instruction zeroInst;
        zeroInst.op = il::Op::ConstF64;
        zeroInst.type = il::Type::F64;
        zeroInst.result = zeroRes;
        zeroInst.immF64 = 0.0;
        emitInst(ilFn, zeroInst);
        thisArgVal = boxValueIfNeeded(Value{zeroRes, il::Type::F64}, ilFn);
    }

    std::vector<il::ValueId> dynOperands;
    dynOperands.push_back(calleeVal.id);
    dynOperands.push_back(thisArgVal.id);

    for (const auto& argPtr : call->args) {
        auto argVal = lowerExpr(*argPtr, ilFn);
        if (!argVal) return std::nullopt;
        auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
        dynOperands.push_back(argBoxed.id);
    }

    il::ValueId callRes = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::DynamicCall;
    inst.type = il::Type::Dynamic;
    inst.result = callRes;
    inst.operands = std::move(dynOperands);
    emitInst(ilFn, inst);
    return Value{callRes, il::Type::Dynamic};
}

}  // namespace bronze::lower
