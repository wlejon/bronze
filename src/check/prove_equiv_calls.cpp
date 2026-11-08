#include "check/prove_equiv_sym.h"

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

SymEvalOutcome SymbolicIlBuilder::invokeCallee(const il::Function& callee,
                                               const il::Instruction& inst,
                                               size_t startOp,
                                               const std::string& thisPath,
                                               uint32_t envId,
                                               uint32_t inlineDepth,
                                               SymState& state,
                                               bool writeScalarRet,
                                               bool isConstructCall) {
    std::vector<BVNodeId> callArgs;
    std::vector<std::string> callPaths;
    std::vector<bool> callUndef;
    for (size_t k = startOp; k < inst.operands.size(); ++k) {
        callArgs.push_back(toBV64(getVal(state, inst.operands[k])));
        callPaths.push_back(state.heap.lookupObjPath(inst.operands[k]));
        callUndef.push_back(state.undefValues.count(inst.operands[k]) > 0);
    }
    auto sub = evalFunction(callee, callArgs, inlineDepth + 1, &state.heap,
                            thisPath.empty() ? "this" : thisPath, &callPaths, envId, &callUndef, isConstructCall);
    if (!sub.supported || !sub.hasReturn) {
        return {false, sub.unsupportedReason.empty() ? "callee hit loop unroll bound" : sub.unsupportedReason,
                false, zero64_, {}, {}, zero1_};
    }
    BVNodeId calleeThrew = (sub.threwNode != 0) ? sub.threwNode : zero1_;
    if (calleeThrew < ctx_.nodes().size() && ctx_.nodes()[calleeThrew].op == BVOp::Const &&
        ctx_.nodes()[calleeThrew].imm != 0) {
        return {true, {}, true, zero64_, std::move(state.heap), {}, one1_};
    }
    if (calleeThrew != zero1_) {
        state.threwCond = (state.threwCond == 0 || state.threwCond == zero1_)
                              ? calleeThrew
                              : ctx_.make_binary(BVOp::Or, state.threwCond, calleeThrew);
        SymbolicHeapState merged = SymbolicHeapState::mergeBranches(
            heapTable_, calleeThrew, state.heap, sub.finalHeap);
        state.heap.writes = std::move(merged.writes);
    } else {
        state.heap.writes = std::move(sub.finalHeap.writes);
    }
    if (sub.exceededBoundNode != 0 && sub.exceededBoundNode != zero1_) {
        state.exceededCond = (state.exceededCond == 0 || state.exceededCond == zero1_)
                                 ? sub.exceededBoundNode
                                 : ctx_.make_binary(BVOp::Or, state.exceededCond, sub.exceededBoundNode);
    }
    state.heap.propFuncRefs = std::move(sub.finalHeap.propFuncRefs);
    state.heap.localObjCounter = sub.finalHeap.localObjCounter;
    if (writeScalarRet) state.values[inst.result] = sub.returnVal;
    if (!sub.returnObjPath.empty() && inst.result != il::kNoValue) {
        state.heap.objPaths[inst.result] = sub.returnObjPath;
        state.values[inst.result] = sub.returnVal;
    }
    return {true, {}, false, zero64_, {}, {}, zero1_};
}

bool SymbolicIlBuilder::tryEvalGlobalCall(const std::string& gName,
                                          const std::string& method,
                                          const il::Instruction& inst,
                                          SymState& state,
                                          std::string& outErr) {
    std::vector<BVNodeId> opVals;
    for (il::ValueId opId : inst.operands) opVals.push_back(toBV64(getVal(state, opId)));
    BVNodeId gOut = zero64_;
    std::vector<BVNodeId> gArr;
    if (!evalGlobalMethodCallSymbolic(ctx_, heapTable_, state.heap, gName, method,
                                      inst.operands, opVals, inst.result, gOut, &gArr, &outErr)) {
        return false;
    }
    if (outErr.empty()) {
        state.values[inst.result] = gOut;
        if (!gArr.empty()) state.arrayValues[inst.result] = std::move(gArr);
    }
    return true;
}

SymEvalOutcome SymbolicIlBuilder::evalConstructOp(const il::Instruction& inst,
                                                  uint32_t inlineDepth,
                                                  SymState& state) {
    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
    std::string localPath = state.heap.lookupObjPath(inst.result);
    if (!inst.operands.empty() && state.funcRefs.count(inst.operands[0])) {
        uint32_t ctorIdx = state.funcRefs[inst.operands[0]];
        state.ctorOf[inst.result] = ctorIdx;
        uint32_t ctorEnv = state.funcEnvs.count(inst.operands[0]) ? state.funcEnvs[inst.operands[0]] : 0;
        if (ctorIdx < mod_.functions.size() && !mod_.functions[ctorIdx].isExternal) {
            return invokeCallee(mod_.functions[ctorIdx], inst, 1, localPath,
                                ctorEnv, inlineDepth, state, false, true);
        }
    }
    if (inst.operands.size() >= 2) {
        il::ValueId arg1 = inst.operands[1];
        auto arrIt = state.arrayValues.find(arg1);
        std::string arg1Path = state.heap.lookupObjPath(arg1);
        if (arrIt != state.arrayValues.end()) {
            state.arrayValues[inst.result] = arrIt->second;
            state.heap.evalPropSet(heapTable_, inst.result, "length", ctx_.make_const(arrIt->second.size(), 64));
            for (size_t idx = 0; idx < arrIt->second.size(); ++idx) {
                state.heap.evalPropSet(heapTable_, inst.result, std::to_string(idx), arrIt->second[idx]);
            }
        } else if (!arg1Path.empty() && state.heap.writes.count(arg1Path + ".length")) {
            state.heap.objPaths[inst.result] = arg1Path;
            state.values[inst.result] = toBV64(getVal(state, arg1));
        } else {
            state.heap.evalPropSet(heapTable_, inst.result, "length", toBV64(getVal(state, arg1)), arg1);
            if (inst.operands.size() >= 3) {
                state.heap.evalPropSet(heapTable_, inst.result, "sign", toBV64(getVal(state, inst.operands[2])), inst.operands[2]);
            }
        }
    } else {
        state.heap.evalPropSet(heapTable_, inst.result, "length", zero64_);
    }
    return {true, {}, false, zero64_, {}, {}, zero1_};
}

SymEvalOutcome SymbolicIlBuilder::evalMethodCallOp(const il::Instruction& inst,
                                                   uint32_t inlineDepth,
                                                   SymState& state) {
    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
        return {false, "unsupported dynamic method call", false, zero64_, {}, {}, zero1_};
    }
    const std::string& method = mod_.keyConstants[inst.keyIndex];
    if (state.globalRefs.count(inst.operands[0])) {
        std::string gErr;
        if (tryEvalGlobalCall(state.globalRefs[inst.operands[0]], method, inst, state, gErr)) {
            if (!gErr.empty()) return {false, gErr, false, zero64_, {}, {}, zero1_};
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    if ((method == "charCodeAt" || method == "codePointAt" || method == "charAt") && inst.operands.size() >= 2) {
        BVNodeId idx = toBV64(getVal(state, inst.operands[1]));
        auto arrIt = state.arrayValues.find(inst.operands[0]);
        if (arrIt != state.arrayValues.end() && !arrIt->second.empty()) {
            BVNodeId res = zero64_;
            for (size_t i = 0; i < arrIt->second.size(); ++i) {
                res = ctx_.make_ite(ctx_.make_binary(BVOp::Eq, idx, ctx_.make_const(i, 64)), arrIt->second[i], res);
            }
            state.values[inst.result] = res;
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
        BVNodeId chVal = zero64_;
        if (state.heap.evalElemGet(heapTable_, inst.operands[0], idx, inst.result, chVal)) {
            state.values[inst.result] = ctx_.make_binary(BVOp::And, chVal, ctx_.make_const(0xFFFF, 64));
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    if (method == "push" && inst.operands.size() >= 2) {
        BVNodeId elem = toBV64(getVal(state, inst.operands[1])), curLen = zero64_;
        if (state.arrayValues.count(inst.operands[0])) state.arrayValues[inst.operands[0]].push_back(elem);
        if (state.heap.evalPropGet(heapTable_, inst.operands[0], "length", il::kNoValue, curLen)) {
            state.heap.evalElemSet(heapTable_, inst.operands[0], curLen, elem, inst.operands[1]);
            BVNodeId newLen = ctx_.make_binary(BVOp::Add, curLen, one64_);
            state.heap.evalPropSet(heapTable_, inst.operands[0], "length", newLen);
            state.values[inst.result] = newLen;
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    if (method == "pop" && !inst.operands.empty()) {
        auto arrIt = state.arrayValues.find(inst.operands[0]);
        if (arrIt != state.arrayValues.end() && !arrIt->second.empty()) {
            state.values[inst.result] = arrIt->second.back();
            arrIt->second.pop_back();
        }
        BVNodeId curLen = zero64_;
        if (state.heap.evalPropGet(heapTable_, inst.operands[0], "length", il::kNoValue, curLen)) {
            BVNodeId newLen = ctx_.make_binary(BVOp::Sub, curLen, one64_), lastElem = zero64_;
            bool isU = false;
            state.heap.evalElemGet(heapTable_, inst.operands[0], newLen, inst.result, lastElem, &isU);
            state.heap.evalPropSet(heapTable_, inst.operands[0], "length", newLen);
            if (isU) state.undefValues.insert(inst.result);
            state.values[inst.result] = lastElem;
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    if ((method == "slice" || method == "subarray") && !inst.operands.empty()) {
        state.values[inst.result] = toBV64(getVal(state, inst.operands[0]));
        state.heap.propagateAlias(inst.operands[0], inst.result);
        return {true, {}, false, zero64_, {}, {}, zero1_};
    }
    std::string recvPath = state.heap.lookupObjPath(inst.operands[0]);
    const il::Function* callee = (inst.directTarget != il::Instruction::kNoDirectTarget &&
                                  inst.directTarget < mod_.functions.size())
                                     ? &mod_.functions[inst.directTarget]
                                     : findMethodCalleeInModule(mod_, state.heap, recvPath, method);
    if (callee && !callee->isExternal) {
        return invokeCallee(*callee, inst, 1, recvPath, 0, inlineDepth, state);
    }
    if (inModuleInit_) {
        state.values[inst.result] = zero64_;
        return {true, {}, false, zero64_, {}, {}, zero1_};
    }
    return {false, "unsupported method call: " + method, false, zero64_, {}, {}, zero1_};
}

SymEvalOutcome SymbolicIlBuilder::evalDynamicCallOp(const il::Instruction& inst,
                                                    uint32_t inlineDepth,
                                                    SymState& state) {
    if (!inst.operands.empty() && state.globalRefs.count(inst.operands[0])) {
        std::string gErr;
        if (tryEvalGlobalCall(state.globalRefs[inst.operands[0]], "", inst, state, gErr)) {
            if (!gErr.empty()) return {false, gErr, false, zero64_, {}, {}, zero1_};
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
    }
    if (inst.operands.empty() || !state.funcRefs.count(inst.operands[0])) {
        if (inModuleInit_) {
            state.values[inst.result] = zero64_;
            return {true, {}, false, zero64_, {}, {}, zero1_};
        }
        return {false, "unsupported IL operation: call.dynamic", false, zero64_, {}, {}, zero1_};
    }
    uint32_t calleeIdx = state.funcRefs[inst.operands[0]];
    if (calleeIdx >= mod_.functions.size() || mod_.functions[calleeIdx].isExternal) {
        return {false, "external dynamic callee", false, zero64_, {}, {}, zero1_};
    }
    uint32_t capEnv = state.funcEnvs.count(inst.operands[0]) ? state.funcEnvs[inst.operands[0]] : 0;
    std::string thisPath = (inst.operands.size() > 1) ? state.heap.lookupObjPath(inst.operands[1]) : std::string{};
    return invokeCallee(mod_.functions[calleeIdx], inst, 2, thisPath, capEnv, inlineDepth, state);
}

SymEvalOutcome SymbolicIlBuilder::evalDirectCallOp(const il::Instruction& inst,
                                                   uint32_t inlineDepth,
                                                   SymState& state) {
    if (inst.calleeIndex >= mod_.functions.size()) {
        return {false, "invalid callee index", false, zero64_, {}, {}, zero1_};
    }
    const il::Function& callee = mod_.functions[inst.calleeIndex];
    uint32_t calleeEnvId = 0;
    if (callee.needsEnv && !inst.operands.empty()) {
        uint32_t cur = state.envIds.count(inst.operands[0]) ? state.envIds[inst.operands[0]] : 0;
        uint32_t hops = (inst.callEnvHops != il::Instruction::kNoEnvHops) ? inst.callEnvHops : 0;
        for (uint32_t h = 0; h < hops; ++h) {
            cur = state.envParent.count(cur) ? state.envParent[cur] : 0;
        }
        calleeEnvId = cur;
    }
    std::string thisPath = "this";
    if (callee.needsThis && (callee.needsEnv ? 1u : 0u) < inst.operands.size()) {
        std::string p = state.heap.lookupObjPath(inst.operands[callee.needsEnv ? 1 : 0]);
        if (!p.empty()) thisPath = std::move(p);
    }
    size_t startOp = (inst.operands.size() == callee.params.size())
                         ? callee.firstSourceParam()
                         : ((inst.callEnvHops != il::Instruction::kNoEnvHops && !inst.operands.empty()) ? 1 : 0);
    return invokeCallee(callee, inst, startOp, thisPath, calleeEnvId, inlineDepth, state);
}

}  // namespace bronze::check
