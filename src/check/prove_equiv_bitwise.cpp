#include "check/prove_equiv_sym.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

SymEvalOutcome SymbolicIlBuilder::evalBlockPath(const il::Function& fn,
                                                il::BlockId blockId,
                                                SymState state,
                                                uint32_t steps,
                                                uint32_t inlineDepth) {
    const uint32_t maxSteps = inModuleInit_ ? kMaxModuleInitSteps : kMaxBlockSteps;
    const uint32_t maxVisits = inModuleInit_ ? kMaxModuleInitVisits : kMaxTotalBlockVisits;
    std::vector<uint8_t> blockInCycle;
    const uint32_t rawLoopBound = inModuleInit_ ? 64u : computeFunctionSymbolicLoopBound(fn, blockInCycle);
    const uint32_t symLoopBound = (inlineDepth > 0 && !inModuleInit_) ? std::min(rawLoopBound, 2u) : rawLoopBound;
    auto checkBackedgeLimit = [&](il::BlockId srcBlk, il::BlockId dstBlk, SymState& st) -> bool {
        if (dstBlk > srcBlk) return true;
        for (auto& [bId, cnt] : st.loopHeaderVisits) {
            if (bId > dstBlk) cnt = 0;
        }
        uint32_t visits = ++st.loopHeaderVisits[dstBlk];
        if (inModuleInit_) return true;
        uint32_t limit = st.symbolicLoopActive ? symLoopBound : 64u;
        if (visits > limit) {
            hitLoopUnrollBound_ = true;
            if (maxLoopBoundUsed_ == 0 || limit < maxLoopBoundUsed_) maxLoopBoundUsed_ = limit;
            return false;
        }
        return true;
    };
    while (true) {
        if (++totalVisits_ > maxVisits) {
            if (inModuleInit_) {
                saveModuleInitSnapshot(state);
                return {true, {}, true, zero64_, std::move(state.heap), {}, zero1_, zero1_};
            }
            return {false, "symbolic path limit exceeded", false, zero64_, {}, {}, zero1_, zero1_};
        }
        if (steps > maxSteps) {
            if (inModuleInit_) {
                saveModuleInitSnapshot(state);
                return {true, {}, true, zero64_, std::move(state.heap), {}, zero1_, zero1_};
            }
            hitLoopUnrollBound_ = true;
            if (maxLoopBoundUsed_ == 0 || symLoopBound < maxLoopBoundUsed_) maxLoopBoundUsed_ = symLoopBound;
            return {true, "loop unroll bound reached", false, zero64_, {}, {}, zero1_, one1_};
        }
        if (blockId >= fn.blocks.size()) {
            return {false, "invalid block target", false, zero64_, {}, {}, zero1_};
        }

        bool jumpedInPlace = false;
        const il::Block& blk = fn.blocks[blockId];
        for (size_t instIdx = 0; instIdx < blk.instructions.size(); ++instIdx) {
            const auto& inst = blk.instructions[instIdx];
            using Op = il::Op;
            switch (inst.op) {
                case Op::ConstI32:
                    state.values[inst.result] = floatMode_
                        ? makeF64ConstNode(ctx_, static_cast<double>(inst.immI32))
                        : ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64);
                    break;
                case Op::ConstF64: {
                    if (floatMode_) {
                        state.values[inst.result] = makeF64ConstNode(ctx_, inst.immF64);
                        break;
                    }
                    if (std::isnan(inst.immF64) || std::isinf(inst.immF64) || std::trunc(inst.immF64) != inst.immF64) {
                        if (inModuleInit_) { state.values[inst.result] = zero64_; break; }
                        return {false, std::isnan(inst.immF64) || std::isinf(inst.immF64)
                                           ? "non-finite floating-point constant"
                                           : "fractional floating-point constant", false, zero64_, {}, {}, zero1_};
                    }
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immF64)), 64);
                    break;
                }
                case Op::ConstBool:
                    state.values[inst.result] = (inst.immI32 != 0) ? (floatMode_ ? f64One_ : one64_) : zero64_;
                    break;
                case Op::ConstNull:
                case Op::ConstUndefined:
                    state.values[inst.result] = zero64_;
                    state.undefValues.insert(inst.result);
                    break;
                case Op::IsNullish:
                    state.values[inst.result] = (!inst.operands.empty() && state.undefValues.count(inst.operands[0]))
                                                    ? (floatMode_ ? f64One_ : one64_) : zero64_;
                    break;
                case Op::ToInt32:
                    state.values[inst.result] = toSInt32In64(inst.operands.empty() ? zero64_ : getVal(state, inst.operands[0]));
                    break;
                case Op::NumTruthy:
                    state.values[inst.result] = toBV64(toBool1(inst.operands.empty() ? zero64_ : getVal(state, inst.operands[0])));
                    break;
                case Op::Box:
                case Op::Unbox:
                case Op::ToNumeric:
                case Op::ToStr: {
                    if (inst.op == Op::ToStr && heapTable_.numericParams() && !inst.operands.empty() &&
                        !state.heap.isStringValue(inst.operands[0])) {
                        return {false, kNumberToStringReason, false, zero64_, {}, {}, zero1_};
                    }
                    if (!inst.operands.empty()) {
                        il::ValueId src = inst.operands[0];
                        state.values[inst.result] = toBV64(getVal(state, src));
                        state.heap.propagateAlias(src, inst.result);
                        if (state.arrayValues.count(src)) state.arrayValues[inst.result] = state.arrayValues[src];
                        if (state.funcRefs.count(src)) state.funcRefs[inst.result] = state.funcRefs[src];
                        if (state.funcEnvs.count(src)) state.funcEnvs[inst.result] = state.funcEnvs[src];
                        if (state.ctorOf.count(src)) state.ctorOf[inst.result] = state.ctorOf[src];
                        if (state.globalRefs.count(src)) state.globalRefs[inst.result] = state.globalRefs[src];
                        if (state.envIds.count(src)) state.envIds[inst.result] = state.envIds[src];
                        if (state.undefValues.count(src)) state.undefValues.insert(inst.result);
                        if (inst.op == Op::Box) setKind(state, inst.result, kindOf(state, src));
                    } else if (inst.op == Op::Box && inst.keyIndex < mod_.keyConstants.size()) {
                        evalBoxStringLiteral(mod_.keyConstants[inst.keyIndex], inst.result, state);
                    } else {
                        state.values[inst.result] = zero64_;
                    }
                    break;
                }
                case Op::TypeOf: {
                    std::string tag = "number";
                    if (!inst.operands.empty()) {
                        il::ValueId op0 = inst.operands[0];
                        BVNodeId v0 = toBV64(getVal(state, op0));
                        bool isFnTok = (v0 < ctx_.nodes().size() && ctx_.nodes()[v0].op == BVOp::Const &&
                                        (ctx_.nodes()[v0].imm >> 48) == 0x00F0u);
                        if (state.undefValues.count(op0)) tag = "undefined";
                        else if (state.funcRefs.count(op0) || isFnTok) tag = "function";
                        else if (state.heap.isStringValue(op0)) tag = "string";
                    }
                    evalBoxStringLiteral(tag, inst.result, state);
                    break;
                }
                case Op::GetNewTarget:
                    if (state.isConstructCall) {
                        state.values[inst.result] = one64_;
                    } else {
                        state.values[inst.result] = zero64_;
                        state.undefValues.insert(inst.result);
                    }
                    break;
                case Op::InstanceOf: {
                    if (inst.operands.size() >= 2) {
                        uint32_t rhsFn = UINT32_MAX;
                        if (state.funcRefs.count(inst.operands[1])) {
                            rhsFn = state.funcRefs[inst.operands[1]];
                        } else {
                            BVNodeId rv = toBV64(getVal(state, inst.operands[1]));
                            if (rv < ctx_.nodes().size() && ctx_.nodes()[rv].op == BVOp::Const &&
                                (ctx_.nodes()[rv].imm >> 48) == 0x00F0u) {
                                rhsFn = static_cast<uint32_t>(ctx_.nodes()[rv].imm & 0xFFFFFFFFu);
                            }
                        }
                        if (rhsFn != UINT32_MAX) {
                            il::ValueId lhsId = inst.operands[0];
                            if (state.ctorOf.count(lhsId)) {
                                bool match = (state.ctorOf[lhsId] == rhsFn);
                                state.values[inst.result] = match ? (floatMode_ ? f64One_ : one64_) : zero64_;
                                break;
                            }
                            if (state.arrayValues.count(lhsId) || state.heap.isStringValue(lhsId) ||
                                state.undefValues.count(lhsId) ||
                                (state.isConstructCall && state.heap.lookupObjPath(lhsId).rfind("__local_", 0) != 0)) {
                                state.values[inst.result] = zero64_;
                                break;
                            }
                        }
                    }
                    if (inModuleInit_) {
                        state.values[inst.result] = zero64_;
                        break;
                    }
                    return {false, "unsupported IL operation: instanceof on unknown object", false, zero64_, {}, {}, zero1_};
                }
                case Op::ClosureIs:
                case Op::IsNumber:
                case Op::IsDenseArray:
                case Op::IsNumberArray:
                case Op::IsShape:
                case Op::IsArray:
                case Op::IsTypedArray:
                case Op::LayoutEpoch:
                case Op::LayoutFact:
                case Op::MethodIcIs:
                case Op::AccessorIcIs:
                    if (inst.result != il::kNoValue) state.values[inst.result] = one64_;
                    break;
                case Op::ClosureEnv:
                    state.envIds[inst.result] = (!inst.operands.empty() && state.funcEnvs.count(inst.operands[0]))
                                                    ? state.funcEnvs[inst.operands[0]] : 0;
                    state.values[inst.result] = zero64_;
                    break;
                case Op::ClassExtend:
                    if (!inModuleInit_ && (inst.operands.size() < 2 ||
                        (!state.funcRefs.count(inst.operands[1]) && !state.globalRefs.count(inst.operands[1])))) {
                        return {false, "unsupported IL operation: class.extend", false, zero64_, {}, {}, zero1_};
                    }
                    break;
                case Op::PinGuard:
                case Op::CensusRecord:
                case Op::FeedbackRecord:
                case Op::KeepAlive:
                case Op::EnvInitTdz:
                    break;
                case Op::CreateObject:
                    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
                    break;
                case Op::CreateFunction:
                case Op::FunctionRef: {
                    uint32_t capEnv = (inst.op == Op::CreateFunction && !inst.operands.empty() &&
                                       state.envIds.count(inst.operands[0]))
                                          ? state.envIds[inst.operands[0]] : 0;
                    state.funcRefs[inst.result] = inst.calleeIndex;
                    state.funcEnvs[inst.result] = capEnv;
                    state.funcCapturedEnvs[inst.calleeIndex] = capEnv;
                    std::string fnPath = "__fn_" + std::to_string(inst.calleeIndex);
                    state.heap.objPaths[inst.result] = fnPath;
                    uint64_t fnTok = 0x00F0000000000000ULL |
                                     (static_cast<uint64_t>(capEnv & 0xFFFFu) << 32) |
                                     static_cast<uint64_t>(inst.calleeIndex);
                    state.values[inst.result] = ctx_.make_const(fnTok, 64);
                    break;
                }
                case Op::MethodDef: {
                    if (inst.operands.size() >= 2 && inst.keyIndex < mod_.keyConstants.size()) {
                        std::string rPath = state.heap.lookupObjPath(inst.operands[0]);
                        if (!rPath.empty() && state.funcRefs.count(inst.operands[1])) {
                            uint32_t mFn = state.funcRefs[inst.operands[1]];
                            std::string fullKey = rPath + "." + mod_.keyConstants[inst.keyIndex];
                            state.heap.propFuncRefs[fullKey] = mFn;
                            if (state.funcEnvs.count(inst.operands[1])) {
                                state.heap.propFuncEnvs[fullKey] = state.funcEnvs[inst.operands[1]];
                            }
                            if (state.protoToCtor.count(rPath)) {
                                state.funcOwnerCtor[mFn] = state.protoToCtor[rPath];
                            }
                        }
                    }
                    break;
                }
                case Op::PropGet: {
                    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "malformed PropGet", false, zero64_, {}, {}, zero1_};
                    }
                    const std::string& key = mod_.keyConstants[inst.keyIndex];
                    if (key == "constructor" && state.ctorOf.count(inst.operands[0])) {
                        uint32_t cIdx = state.ctorOf[inst.operands[0]];
                        uint32_t capEnv = state.funcCapturedEnvs.count(cIdx) ? state.funcCapturedEnvs[cIdx] : 0;
                        state.funcRefs[inst.result] = cIdx;
                        state.funcEnvs[inst.result] = capEnv;
                        std::string fnPath = "__fn_" + std::to_string(cIdx);
                        state.heap.objPaths[inst.result] = fnPath;
                        uint64_t fnTok = 0x00F0000000000000ULL |
                                         (static_cast<uint64_t>(capEnv & 0xFFFFu) << 32) |
                                         static_cast<uint64_t>(cIdx);
                        state.values[inst.result] = ctx_.make_const(fnTok, 64);
                        break;
                    }
                    auto gIt = state.globalRefs.find(inst.operands[0]);
                    if (gIt != state.globalRefs.end()) {
                        BVNodeId gVal = zero64_;
                        bool isMethodRef = false;
                        std::string gErr;
                        if (!tryEvalGlobalPropGetSymbolic(ctx_, gIt->second, key, floatMode_, gVal, isMethodRef, &gErr)) {
                            return {false, gErr, false, zero64_, {}, {}, zero1_};
                        }
                        if (isMethodRef) {
                            state.globalRefs[inst.result] = gIt->second + "." + key;
                            state.values[inst.result] = one64_;
                        } else {
                            state.values[inst.result] = gVal;
                        }
                        break;
                    }
                    std::string rPath = state.heap.lookupObjPath(inst.operands[0]);
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && key == "length" &&
                        (rPath.empty() || !state.heap.writes.count(rPath + ".length"))) {
                        state.values[inst.result] = ctx_.make_const(arrIt->second.size(), 64);
                        break;
                    }
                    if (!rPath.empty() && state.heap.propFuncRefs.count(rPath + "." + key)) {
                        state.funcRefs[inst.result] = state.heap.propFuncRefs[rPath + "." + key];
                        if (state.heap.propFuncEnvs.count(rPath + "." + key)) {
                            state.funcEnvs[inst.result] = state.heap.propFuncEnvs[rPath + "." + key];
                        }
                    } else if (state.ctorOf.count(inst.operands[0])) {
                        uint32_t cIdx = state.ctorOf[inst.operands[0]];
                        if (state.ctorToProto.count(cIdx)) {
                            std::string pPath = state.ctorToProto[cIdx] + "." + key;
                            if (state.heap.propFuncRefs.count(pPath)) {
                                state.funcRefs[inst.result] = state.heap.propFuncRefs[pPath];
                                if (state.heap.propFuncEnvs.count(pPath)) {
                                    state.funcEnvs[inst.result] = state.heap.propFuncEnvs[pPath];
                                }
                            }
                        }
                    }
                    BVNodeId propVal = zero64_;
                    bool isUndefProp = false;
                    if (!state.heap.evalPropGet(heapTable_, inst.operands[0], key, inst.result, propVal, &isUndefProp)) {
                        if (inModuleInit_) { state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result); break; }
                        return {false, "unsupported PropGet on non-tracked receiver", false, zero64_, {}, {}, zero1_};
                    }
                    if (key == "prototype" && state.funcRefs.count(inst.operands[0])) {
                        uint32_t cIdx = state.funcRefs[inst.operands[0]];
                        std::string pPath = state.heap.lookupObjPath(inst.result);
                        if (!pPath.empty()) {
                            state.protoToCtor[pPath] = cIdx;
                            state.ctorToProto[cIdx] = pPath;
                            state.heap.propFuncRefs[pPath + ".constructor"] = cIdx;
                            state.moduleCtorIdx = cIdx;
                        }
                    }
                    if (propVal < ctx_.nodes().size() && ctx_.nodes()[propVal].op == BVOp::Const &&
                        (ctx_.nodes()[propVal].imm >> 48) == 0x00F0u) {
                        state.funcRefs[inst.result] = static_cast<uint32_t>(ctx_.nodes()[propVal].imm & 0xFFFFFFFFu);
                        state.funcEnvs[inst.result] = static_cast<uint32_t>((ctx_.nodes()[propVal].imm >> 32) & 0xFFFFu);
                    }
                    if (isUndefProp) state.undefValues.insert(inst.result);
                    state.values[inst.result] = propVal;
                    break;
                }
                case Op::PropSet:
                case Op::DefineOwnAttr: {
                    if (inst.operands.size() < 2 || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "malformed PropSet", false, zero64_, {}, {}, zero1_};
                    }
                    const std::string& key = mod_.keyConstants[inst.keyIndex];
                    BVNodeId valNode = toBV64(getVal(state, inst.operands[1]));
                    std::string rPath = state.heap.lookupObjPath(inst.operands[0]);
                    if (!rPath.empty() && state.funcRefs.count(inst.operands[1])) {
                        uint32_t mFn = state.funcRefs[inst.operands[1]];
                        state.heap.propFuncRefs[rPath + "." + key] = mFn;
                        if (state.funcEnvs.count(inst.operands[1])) {
                            state.heap.propFuncEnvs[rPath + "." + key] = state.funcEnvs[inst.operands[1]];
                        }
                        if (key == "constructor") {
                            state.protoToCtor[rPath] = mFn;
                            state.ctorToProto[mFn] = rPath;
                            state.moduleCtorIdx = mFn;
                        } else if (state.protoToCtor.count(rPath)) {
                            state.funcOwnerCtor[mFn] = state.protoToCtor[rPath];
                        }
                    }
                    if (key == "prototype" && state.funcRefs.count(inst.operands[0])) {
                        uint32_t cIdx = state.funcRefs[inst.operands[0]];
                        std::string vPath = state.heap.lookupObjPath(inst.operands[1]);
                        if (!vPath.empty()) {
                            state.protoToCtor[vPath] = cIdx;
                            state.ctorToProto[cIdx] = vPath;
                            state.heap.propFuncRefs[vPath + ".constructor"] = cIdx;
                            state.moduleCtorIdx = cIdx;
                            std::string pref = vPath + ".";
                            for (const auto& [pk, mFn] : state.heap.propFuncRefs) {
                                if (pk.rfind(pref, 0) == 0 && pk != vPath + ".constructor") {
                                    state.funcOwnerCtor[mFn] = cIdx;
                                }
                            }
                        }
                    }
                    auto arrIt = state.arrayValues.find(inst.operands[1]);
                    if (!rPath.empty() && arrIt != state.arrayValues.end()) {
                        state.heap.writes[rPath + "." + key + ".length"] =
                            ctx_.make_const(arrIt->second.size(), 64);
                        for (size_t idx = 0; idx < arrIt->second.size(); ++idx) {
                            state.heap.writes[rPath + "." + key + "." + std::to_string(idx)] =
                                arrIt->second[idx];
                        }
                    }
                    if (!state.heap.evalPropSet(heapTable_, inst.operands[0], key, valNode, inst.operands[1])) {
                        if (inModuleInit_) break;
                        return {false, "unsupported PropSet on non-tracked receiver", false, zero64_, {}, {}, zero1_};
                    }
                    if (key == "length") state.arrayValues.erase(inst.operands[0]);
                    if (inst.result != il::kNoValue) state.values[inst.result] = valNode;
                    break;
                }
                case Op::ConcatBegin:
                case Op::ConcatAppend:
                    if (heapTable_.numericParams() &&
                        std::any_of(inst.operands.begin(), inst.operands.end(),
                                    [&](il::ValueId v) { return !state.heap.isStringValue(v); })) {
                        return {false, kNumberConcatReason, false, zero64_, {}, {}, zero1_};
                    }
                    if (inst.op == Op::ConcatAppend) {
                        if (inst.operands.size() >= 2) {
                            evalStringConcatOp(inst.operands[0], inst.operands[1], inst.result, state);
                        }
                    } else if (inst.operands.empty()) {
                        evalBoxStringLiteral("", inst.result, state);
                    } else if (inst.operands.size() == 1) {
                        state.values[inst.result] = toBV64(getVal(state, inst.operands[0]));
                        state.heap.propagateAlias(inst.operands[0], inst.result);
                    } else {
                        evalStringConcatOp(inst.operands[0], inst.operands[1], inst.result, state);
                    }
                    break;
                case Op::ConcatEnd:
                    if (!inst.operands.empty()) {
                        state.values[inst.result] = toBV64(getVal(state, inst.operands[0]));
                        state.heap.propagateAlias(inst.operands[0], inst.result);
                    }
                    break;
                case Op::NumericStep:
                    state.values[inst.result] = floatMode_
                        ? ctx_.make_binary(BVOp::FAdd, toBV64(getVal(state, inst.operands[0])),
                                           makeF64ConstNode(ctx_, static_cast<double>(inst.immI32)), 64)
                        : ctx_.make_binary(BVOp::Add, toBV64(getVal(state, inst.operands[0])),
                                           ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64));
                    break;
                case Op::Add:
                case Op::Sub:
                case Op::Mul: {
                    if (inst.op == Op::Add && inst.operands.size() >= 2 &&
                        (state.heap.isStringValue(inst.operands[0]) || state.heap.isStringValue(inst.operands[1]))) {
                        if (heapTable_.numericParams() && !(state.heap.isStringValue(inst.operands[0]) &&
                                                            state.heap.isStringValue(inst.operands[1]))) {
                            return {false, kNumberConcatReason, false, zero64_, {}, {}, zero1_};
                        }
                        evalStringConcatOp(inst.operands[0], inst.operands[1], inst.result, state);
                        break;
                    }
                    BVNodeId lhs = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId rhs = toBV64(getVal(state, inst.operands[1]));
                    if (floatMode_) {
                        BVNodeId fOut = zero64_;
                        evalFloatBinaryOpSymbolic(ctx_, inst.op, lhs, rhs, fOut);
                        state.values[inst.result] = fOut;
                        break;
                    }
                    BVOp bvOp = (inst.op == Op::Add) ? BVOp::Add : (inst.op == Op::Sub ? BVOp::Sub : BVOp::Mul);
                    auto lIv = getSignedInterval64(ctx_, lhs, heapTable_.domainMin(), heapTable_.domainMax(), 16, &state.heap.refinedIntervals);
                    auto rIv = getSignedInterval64(ctx_, rhs, heapTable_.domainMin(), heapTable_.domainMax(), 16, &state.heap.refinedIntervals);
                    if (bvOp == BVOp::Mul) {
                        auto maxAbs = [](int64_t lo, int64_t hi) -> uint64_t {
                            uint64_t aLo = (lo == INT64_MIN) ? (1ULL << 63) : static_cast<uint64_t>(std::abs(lo));
                            uint64_t aHi = (hi == INT64_MIN) ? (1ULL << 63) : static_cast<uint64_t>(std::abs(hi));
                            return std::max(aLo, aHi);
                        };
                        uint64_t maxAbsL = maxAbs(lIv.smin, lIv.smax);
                        uint64_t maxAbsR = maxAbs(rIv.smin, rIv.smax);
                        if (maxAbsR > 0 && maxAbsL > (1ULL << 62) / maxAbsR) {
                            if (inModuleInit_) { state.values[inst.result] = zero64_; break; }
                            return {false, "integer multiplication can exceed 2^53 safe integer range", false, zero64_, {}, {}, zero1_};
                        }
                        BVNodeId prod = ctx_.make_binary(BVOp::Mul, lhs, rhs);
                        if (maxAbsR > 0 && maxAbsL > (1ULL << 53) / maxAbsR) {
                            prod = roundSignedInt64AsDouble(ctx_, prod);
                        }
                        state.values[inst.result] = prod;
                        break;
                    }
                    static constexpr int64_t kMaxSafeAddOperand = (1LL << 52);
                    if (lIv.smin < -kMaxSafeAddOperand || lIv.smax > kMaxSafeAddOperand ||
                        rIv.smin < -kMaxSafeAddOperand || rIv.smax > kMaxSafeAddOperand) {
                        if (inModuleInit_) { state.values[inst.result] = zero64_; break; }
                        return {false, "integer addition/subtraction can exceed 2^53 safe integer range", false, zero64_, {}, {}, zero1_};
                    }
                    if (bvOp == BVOp::Add) {
                        const auto& ln = ctx_.nodes()[lhs];
                        const auto& rn = ctx_.nodes()[rhs];
                        if (ln.op == BVOp::SExt && rn.op == BVOp::SExt) {
                            const auto& lc = ctx_.nodes()[ln.a];
                            const auto& rc = ctx_.nodes()[rn.a];
                            bool andOr = (lc.op == BVOp::And && rc.op == BVOp::Or) ||
                                         (lc.op == BVOp::Or && rc.op == BVOp::And);
                            if (andOr && ((lc.a == rc.a && lc.b == rc.b) || (lc.a == rc.b && lc.b == rc.a))) {
                                BVNodeId u = lc.a;
                                BVNodeId v = lc.b;
                                uint32_t extBits = 64 - lc.width;
                                lhs = ctx_.make_sext(u, extBits);
                                rhs = ctx_.make_sext(v, extBits);
                            }
                        }
                    }
                    if (inst.operands.size() >= 2 && bvOp == BVOp::Add) {
                        if (state.heap.divResults.count(inst.operands[1])) {
                            const auto& d = state.heap.divResults[inst.operands[1]];
                            BVNodeId off = (d.intOffset != 0) ? ctx_.make_binary(BVOp::Add, lhs, d.intOffset) : lhs;
                            state.heap.divResults[inst.result] = {d.num, d.den, off};
                        } else if (state.heap.divResults.count(inst.operands[0])) {
                            const auto& d = state.heap.divResults[inst.operands[0]];
                            BVNodeId off = (d.intOffset != 0) ? ctx_.make_binary(BVOp::Add, d.intOffset, rhs) : rhs;
                            state.heap.divResults[inst.result] = {d.num, d.den, off};
                        }
                    }
                    state.values[inst.result] = ctx_.make_binary(bvOp, lhs, rhs);
                    break;
                }
                case Op::Pow: {
                    bool ok = false;
                    BVNodeId lhs = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId rhs = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId r = floatMode_ ? evalFloatPowSymbolic(ctx_, lhs, rhs, ok)
                                            : evalPowSymbolic(ctx_, lhs, rhs, ok,
                                                              heapTable_.domainMin(), heapTable_.domainMax());
                    if (!ok) return {false, "unsupported symbolic exponentiation", false, zero64_, {}, {}, zero1_};
                    state.values[inst.result] = r;
                    break;
                }
                case Op::Div:
                case Op::Mod: {
                    BVNodeId divVal = zero64_;
                    std::string divErr;
                    BVNodeId lhs = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId rhs = toBV64(getVal(state, inst.operands[1]));
                    if (floatMode_) {
                        if (!evalFloatBinaryOpSymbolic(ctx_, inst.op, lhs, rhs, divVal, &divErr)) {
                            return {false, divErr, false, zero64_, {}, {}, zero1_};
                        }
                    } else if (!evalDivOrModSymbolic(ctx_, mod_, fn, blk, instIdx, lhs, rhs, state.heap, divVal, &divErr,
                                                     heapTable_.domainMin(), heapTable_.domainMax())) {
                        return {false, divErr, false, zero64_, {}, {}, zero1_};
                    }
                    state.values[inst.result] = divVal;
                    break;
                }
                case Op::Neg:
                    state.values[inst.result] = ctx_.make_unary(
                        floatMode_ ? BVOp::FNeg : BVOp::Neg, toBV64(getVal(state, inst.operands[0])), 64);
                    break;
                case Op::MathImul:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor: {
                    BVNodeId a64 = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b64 = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId a32 = floatMode_ ? evalFloatToInt32Node(ctx_, a64) : ctx_.make_extract(a64, 31, 0);
                    BVNodeId b32 = floatMode_ ? evalFloatToInt32Node(ctx_, b64) : ctx_.make_extract(b64, 31, 0);
                    BVOp bvOp = (inst.op == Op::MathImul) ? BVOp::Mul
                                : (inst.op == Op::BitAnd) ? BVOp::And
                                : (inst.op == Op::BitOr)  ? BVOp::Or : BVOp::Xor;
                    BVNodeId r32 = ctx_.make_binary(bvOp, a32, b32);
                    state.values[inst.result] = floatMode_ ? ctx_.make_unary(BVOp::SiToFp, r32, 64) : ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::BitNot: {
                    BVNodeId a64 = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId a32 = floatMode_ ? evalFloatToInt32Node(ctx_, a64) : ctx_.make_extract(a64, 31, 0);
                    BVNodeId r32 = ctx_.make_unary(BVOp::Not, a32);
                    state.values[inst.result] = floatMode_ ? ctx_.make_unary(BVOp::SiToFp, r32, 64) : ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::Shl:
                case Op::Shr:
                case Op::UShr: {
                    BVNodeId a64 = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId b64 = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId a32 = floatMode_ ? evalFloatToInt32Node(ctx_, a64) : ctx_.make_extract(a64, 31, 0);
                    BVNodeId b32 = floatMode_ ? evalFloatToInt32Node(ctx_, b64) : ctx_.make_extract(b64, 31, 0);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, ctx_.make_const(31, 32));
                    BVOp shOp = (inst.op == Op::Shl) ? BVOp::Shl
                                                     : (inst.op == Op::Shr ? BVOp::AShr : BVOp::LShr);
                    BVNodeId r32 = ctx_.make_binary(shOp, a32, sh32);
                    if (floatMode_) {
                        state.values[inst.result] = (inst.op == Op::UShr)
                            ? ctx_.make_unary(BVOp::SiToFp, ctx_.make_zext(r32, 32), 64)
                            : ctx_.make_unary(BVOp::SiToFp, r32, 64);
                    } else {
                        state.values[inst.result] = (inst.op == Op::UShr) ? ctx_.make_zext(r32, 32) : ctx_.make_sext(r32, 32);
                    }
                    break;
                }
                case Op::CmpEq:
                case Op::StrictEq:
                case Op::LooseEq:
                case Op::CmpNe: {
                    bool isEq = (inst.op != Op::CmpNe);
                    BVNodeId tVal = floatMode_ ? f64One_ : one64_;
                    bool u0 = state.undefValues.count(inst.operands[0]) > 0;
                    bool u1 = state.undefValues.count(inst.operands[1]) > 0;
                    if (u0 != u1) { state.values[inst.result] = isEq ? zero64_ : tVal; break; }
                    if (u0 && u1) { state.values[inst.result] = isEq ? tVal : zero64_; break; }
                    BVNodeId a = toBV64(getVal(state, inst.operands[0])), b = toBV64(getVal(state, inst.operands[1]));
                    BVOp cmpOp = floatMode_ ? (isEq ? BVOp::FEq : BVOp::FNe) : (isEq ? BVOp::Eq : BVOp::Ne);
                    state.values[inst.result] = toBV64(ctx_.make_binary(cmpOp, a, b, 1));
                    state.heap.recordObjComparison(inst.result, inst.operands[0], inst.operands[1], isEq);
                    break;
                }
                case Op::CmpLt: case Op::RelLt: case Op::CmpLe: case Op::RelLe:
                case Op::CmpGt: case Op::RelGt: case Op::CmpGe: case Op::RelGe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0])), b = toBV64(getVal(state, inst.operands[1]));
                    bool swapOps = (inst.op == Op::CmpGt || inst.op == Op::RelGt || inst.op == Op::CmpGe || inst.op == Op::RelGe);
                    bool isLe = (inst.op == Op::CmpLe || inst.op == Op::RelLe || inst.op == Op::CmpGe || inst.op == Op::RelGe);
                    BVOp cmpOp = floatMode_ ? (isLe ? BVOp::FLe : BVOp::FLt) : (isLe ? BVOp::Sle : BVOp::Slt);
                    state.values[inst.result] = toBV64(ctx_.make_binary(cmpOp, swapOps ? b : a, swapOps ? a : b, 1));
                    break;
                }
                case Op::CreateArray: {
                    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
                    state.heap.evalPropSet(heapTable_, inst.result, "length",
                                           ctx_.make_const(static_cast<uint64_t>(std::max(0, inst.immI32)), 64));
                    state.arrayValues[inst.result] = {};
                    break;
                }
                case Op::ArrayAppend: {
                    if (inst.operands.size() >= 2) {
                        BVNodeId elem = toBV64(getVal(state, inst.operands[1]));
                        auto& vec = state.arrayValues[inst.operands[0]];
                        state.heap.evalPropSet(heapTable_, inst.operands[0], std::to_string(vec.size()), elem, inst.operands[1]);
                        vec.push_back(elem);
                        state.heap.evalPropSet(heapTable_, inst.operands[0], "length", ctx_.make_const(vec.size(), 64));
                    }
                    break;
                }
                case Op::ArraySpread:
                    evalArraySpreadOp(inst, state);
                    break;
                case Op::ElemGet:
                case Op::ElemGetTyped: {
                    if (inst.operands.size() < 2) return {false, "malformed ElemGet", false, zero64_, {}, {}, zero1_};
                    BVNodeId idx = toBV64(getVal(state, inst.operands[1]));
                    if (state.heap.isStringValue(inst.operands[1])) {
                        BoundedSeq sSeq;
                        loadBoundedSeq(state, inst.operands[1], /*asString=*/true, sSeq);
                        idx = packBoundedStringNode(ctx_, sSeq.len, sSeq.elems);
                    }
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && !arrIt->second.empty()) {
                        const auto& elems = arrIt->second;
                        if (idx < ctx_.nodes().size() && ctx_.nodes()[idx].op == BVOp::Const) {
                            int64_t cIdx = static_cast<int64_t>(ctx_.nodes()[idx].imm);
                            BVNodeId ev = (cIdx >= 0 && static_cast<size_t>(cIdx) < elems.size())
                                              ? elems[static_cast<size_t>(cIdx)] : zero64_;
                            state.values[inst.result] = ev;
                            if (cIdx < 0 || static_cast<size_t>(cIdx) >= elems.size()) state.undefValues.insert(inst.result);
                            if (ev < ctx_.nodes().size() && ctx_.nodes()[ev].op == BVOp::Const &&
                                (ctx_.nodes()[ev].imm >> 48) == 0x00F0u) {
                                state.funcRefs[inst.result] = static_cast<uint32_t>(ctx_.nodes()[ev].imm & 0xFFFFFFFFu);
                                state.funcEnvs[inst.result] = static_cast<uint32_t>((ctx_.nodes()[ev].imm >> 32) & 0xFFFFu);
                            }
                            break;
                        }
                        BVNodeId res = zero64_;
                        for (size_t i = 0; i < elems.size(); ++i) {
                            res = ctx_.make_ite(ctx_.make_binary(BVOp::Eq, idx, ctx_.make_const(i, 64)), elems[i], res);
                        }
                        state.values[inst.result] = res;
                        break;
                    }
                    BVNodeId elemVal = zero64_;
                    bool isUndefElem = false;
                    if (!state.heap.evalElemGet(heapTable_, inst.operands[0], idx, inst.result, elemVal, &isUndefElem)) {
                        return {false, "unsupported dynamic array ElemGet", false, zero64_, {}, {}, zero1_};
                    }
                    if (isUndefElem) state.undefValues.insert(inst.result);
                    if (elemVal < ctx_.nodes().size() && ctx_.nodes()[elemVal].op == BVOp::Const &&
                        (ctx_.nodes()[elemVal].imm >> 48) == 0x00F0u) {
                        state.funcRefs[inst.result] = static_cast<uint32_t>(ctx_.nodes()[elemVal].imm & 0xFFFFFFFFu);
                        state.funcEnvs[inst.result] = static_cast<uint32_t>((ctx_.nodes()[elemVal].imm >> 32) & 0xFFFFu);
                    }
                    state.values[inst.result] = elemVal;
                    break;
                }
                case Op::ElemSet:
                case Op::ElemSetTyped: {
                    if (inst.operands.size() < 3) return {false, "malformed ElemSet", false, zero64_, {}, {}, zero1_};
                    BVNodeId idxNode = toBV64(getVal(state, inst.operands[1]));
                    BVNodeId valNode = toBV64(getVal(state, inst.operands[2]));
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && idxNode < ctx_.nodes().size() &&
                        ctx_.nodes()[idxNode].op == BVOp::Const) {
                        int64_t cIdx = static_cast<int64_t>(ctx_.nodes()[idxNode].imm);
                        if (cIdx >= 0 && cIdx < 512) {
                            if (static_cast<size_t>(cIdx) >= arrIt->second.size()) {
                                arrIt->second.resize(static_cast<size_t>(cIdx) + 1, zero64_);
                            }
                            arrIt->second[static_cast<size_t>(cIdx)] = valNode;
                        }
                    }
                    if (!state.heap.evalElemSet(heapTable_, inst.operands[0], idxNode, valNode, inst.operands[2])) {
                        if (inModuleInit_) break;
                        return {false, "unsupported ElemSet on non-tracked receiver", false, zero64_, {}, {}, zero1_};
                    }
                    if (inst.result != il::kNoValue) state.values[inst.result] = valNode;
                    break;
                }
                case Op::MathUnary: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if (floatMode_) {
                        if (kind == il::MathUnaryFn::Abs) {
                            state.values[inst.result] = ctx_.make_unary(BVOp::FAbs, a, 64);
                        } else if (kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil) {
                            state.values[inst.result] = (kind == il::MathUnaryFn::Ceil)
                                ? evalFloatCeilSymbolic(ctx_, a)
                                : evalFloatFloorSymbolic(ctx_, a);
                        } else if (kind == il::MathUnaryFn::Trunc) {
                            state.values[inst.result] = evalFloatTruncSymbolic(ctx_, a);
                        } else if (kind == il::MathUnaryFn::Round) {
                            state.values[inst.result] = evalFloatRoundSymbolic(ctx_, a);
                        } else if (kind == il::MathUnaryFn::Sqrt) {
                            state.values[inst.result] = ctx_.make_unary(BVOp::FSqrt, a, 64);
                        } else if (kind == il::MathUnaryFn::Min || kind == il::MathUnaryFn::Max) {
                            BVNodeId acc = a;
                            for (size_t k = 1; k < inst.operands.size(); ++k) {
                                BVNodeId rhs = toBV64(getVal(state, inst.operands[k]));
                                acc = ctx_.make_binary(kind == il::MathUnaryFn::Min ? BVOp::FMin : BVOp::FMax, acc, rhs, 64);
                            }
                            state.values[inst.result] = acc;
                        } else {
                            return {false, "unsupported transcendental MathUnary", false, zero64_, {}, {}, zero1_};
                        }
                        break;
                    }
                    if (kind == il::MathUnaryFn::Abs) {
                        state.values[inst.result] = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, a, zero64_),
                                                                  ctx_.make_unary(BVOp::Neg, a), a);
                    } else if (kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil) {
                        state.values[inst.result] = evalFloorOrCeilSymbolic(
                            ctx_, state.heap, inst.operands[0], a, kind == il::MathUnaryFn::Ceil,
                            heapTable_.domainMin(), heapTable_.domainMax());
                    } else if (kind == il::MathUnaryFn::Trunc) {
                        state.values[inst.result] = a;
                    } else if (kind == il::MathUnaryFn::Min || kind == il::MathUnaryFn::Max) {
                        BVNodeId acc = a;
                        for (size_t k = 1; k < inst.operands.size(); ++k) {
                            BVNodeId rhs = toBV64(getVal(state, inst.operands[k]));
                            BVNodeId pick = (kind == il::MathUnaryFn::Min) ? ctx_.make_binary(BVOp::Slt, rhs, acc)
                                                                           : ctx_.make_binary(BVOp::Slt, acc, rhs);
                            acc = ctx_.make_ite(pick, rhs, acc);
                        }
                        state.values[inst.result] = acc;
                    } else {
                        return {false, "unsupported transcendental MathUnary", false, zero64_, {}, {}, zero1_};
                    }
                    break;
                }
                case Op::GlobalGet:
                case Op::ResolveName: {
                    SymEvalOutcome sub = evalGlobalGetOp(inst, state);
                    if (!sub.supported) return sub;
                    break;
                }
                case Op::Construct: {
                    auto sub = evalConstructOp(inst, inlineDepth, state);
                    if (!sub.supported || sub.hasReturn) return sub;
                    break;
                }
                case Op::SuperCall: {
                    il::ValueId thisParamId = static_cast<il::ValueId>(fn.needsEnv ? 1 : 0);
                    state.heap.propagateAlias(thisParamId, inst.result);
                    state.values[inst.result] = toBV64(getVal(state, thisParamId));
                    if (inst.operands.size() >= 3) {
                        state.heap.evalPropSet(heapTable_, thisParamId, "length", toBV64(getVal(state, inst.operands[2])), inst.operands[2]);
                    }
                    break;
                }
                case Op::MethodCall: {
                    auto sub = evalMethodCallOp(inst, inlineDepth, state);
                    if (!sub.supported || sub.hasReturn) return sub;
                    break;
                }
                case Op::MethodCallSpread: {
                    auto sub = evalCallSpreadOp(inst, true, state);
                    if (!sub.supported || sub.hasReturn) return sub;
                    break;
                }
                case Op::DynamicCall: {
                    auto sub = evalDynamicCallOp(inst, inlineDepth, state);
                    if (!sub.supported || sub.hasReturn) return sub;
                    break;
                }
                case Op::DynamicCallSpread: {
                    auto sub = evalCallSpreadOp(inst, false, state);
                    if (!sub.supported || sub.hasReturn) return sub;
                    break;
                }
                case Op::Call: {
                    auto sub = evalDirectCallOp(inst, inlineDepth, state);
                    if (!sub.supported || sub.hasReturn) return sub;
                    break;
                }
                case Op::EnvCreate: {
                    uint32_t parentId = (!inst.operands.empty() && state.envIds.count(inst.operands[0]))
                                            ? state.envIds[inst.operands[0]] : 0;
                    uint32_t newId = (inModuleInit_ && state.nextEnvId == 1) ? 0 : state.nextEnvId++;
                    if (newId != 0) state.envParent[newId] = parentId;
                    state.envIds[inst.result] = newId;
                    state.values[inst.result] = zero64_;
                    break;
                }
                case Op::ModuleEnvSet:
                    if (!inst.operands.empty()) state.envIds[inst.operands[0]] = 0;
                    break;
                case Op::ModuleEnvGet:
                    state.envIds[inst.result] = 0;
                    state.values[inst.result] = zero64_;
                    break;
                case Op::EnvSet: {
                    if (inst.operands.size() >= 2) {
                        uint64_t key = resolveEnvSlotKey(state, inst.operands[0], inst.envDepth, inst.envIndex);
                        il::ValueId valId = inst.operands[1];
                        state.envSlots[key] = toBV64(getVal(state, valId));
                        state.heap.propagateEnvSet(key, valId);
                        if (state.arrayValues.count(valId)) state.envArrays[key] = state.arrayValues[valId];
                        if (state.funcRefs.count(valId)) state.envFuncRefs[key] = state.funcRefs[valId];
                        if (state.funcEnvs.count(valId)) state.envFuncEnvs[key] = state.funcEnvs[valId];
                        if (state.globalRefs.count(valId)) state.envGlobalRefs[key] = state.globalRefs[valId];
                        if (state.undefValues.count(valId)) state.envUndef.insert(key);
                        else state.envUndef.erase(key);
                    }
                    break;
                }
                case Op::EnvGet:
                case Op::EnvGetTdz: {
                    uint64_t key = resolveEnvSlotKey(state, inst.operands.empty() ? 0 : inst.operands[0],
                                                     inst.envDepth, inst.envIndex);
                    if (!state.envSlots.count(key) && !state.envFuncRefs.count(key) &&
                        !state.envGlobalRefs.count(key) && !state.envArrays.count(key)) {
                        for (uint32_t eid = 0; eid < state.nextEnvId; ++eid) {
                            uint64_t alt = (static_cast<uint64_t>(eid) << 32) | inst.envIndex;
                            if (state.envSlots.count(alt) || state.envFuncRefs.count(alt) ||
                                state.envGlobalRefs.count(alt) || state.envArrays.count(alt)) {
                                key = alt;
                                break;
                            }
                        }
                    }
                    auto it = state.envSlots.find(key);
                    BVNodeId ev = (it != state.envSlots.end()) ? it->second : zero64_;
                    state.values[inst.result] = ev;
                    state.heap.propagateEnvGet(key, inst.result);
                    if (state.envArrays.count(key)) state.arrayValues[inst.result] = state.envArrays[key];
                    if (state.envFuncRefs.count(key)) state.funcRefs[inst.result] = state.envFuncRefs[key];
                    if (state.envFuncEnvs.count(key)) state.funcEnvs[inst.result] = state.envFuncEnvs[key];
                    if (state.envGlobalRefs.count(key)) state.globalRefs[inst.result] = state.envGlobalRefs[key];
                    if (state.envUndef.count(key)) state.undefValues.insert(inst.result);
                    if (std::string gotPath = state.heap.lookupObjPath(inst.result); !gotPath.empty()) {
                        for (const auto& [cv, cIdx] : state.ctorOf) {
                            if (state.heap.lookupObjPath(cv) == gotPath) {
                                state.ctorOf[inst.result] = cIdx;
                                break;
                            }
                        }
                    }
                    if (ev < ctx_.nodes().size() && ctx_.nodes()[ev].op == BVOp::Const &&
                        (ctx_.nodes()[ev].imm >> 48) == 0x00F0u) {
                        state.funcRefs[inst.result] = static_cast<uint32_t>(ctx_.nodes()[ev].imm & 0xFFFFFFFFu);
                        state.funcEnvs[inst.result] = static_cast<uint32_t>((ctx_.nodes()[ev].imm >> 32) & 0xFFFFu);
                    }
                    break;
                }
                case Op::Throw:
                    return {true, {}, true, zero64_, std::move(state.heap), {}, one1_, zero1_};
                case Op::Jump:
                    if (!checkBackedgeLimit(blockId, inst.target.block, state)) {
                        return {true, "loop unroll bound reached", false, zero64_, {}, {}, zero1_, one1_};
                    }
                    applyEdgeArgsInPlace(fn, inst.target, state);
                    blockId = inst.target.block;
                    ++steps;
                    jumpedInPlace = true;
                    break;
                case Op::Branch: {
                    BVNodeId condNode = toBool1(toBV64(getVal(state, inst.operands[0])));
                    int folded = tryFoldRefinedBranchCond(ctx_, condNode, heapTable_.domainMin(),
                                                          heapTable_.domainMax(), state.heap.refinedIntervals);
                    if (folded >= 0) {
                        const il::BlockTarget& edge = (folded != 0) ? inst.target : inst.elseTarget;
                        if (!checkBackedgeLimit(blockId, edge.block, state)) {
                            return {true, "loop unroll bound reached", false, zero64_, {}, {}, zero1_, one1_};
                        }
                        applyEdgeArgsInPlace(fn, edge, state);
                        blockId = edge.block;
                        ++steps;
                        jumpedInPlace = true;
                        break;
                    }
                    if (blockId < blockInCycle.size() && blockInCycle[blockId]) {
                        state.symbolicLoopActive = true;
                    }
                    SymState trueState = state;
                    trueState.heap.unifyBranchAlias(heapTable_, inst.operands[0], condNode, true);
                    applyEdgeArgsInPlace(fn, inst.target, trueState);
                    SymEvalOutcome trueOut;
                    if (!checkBackedgeLimit(blockId, inst.target.block, trueState)) {
                        trueOut = {true, "loop unroll bound reached", false, zero64_, {}, {}, zero1_, one1_};
                    } else {
                        trueOut = evalBlockPath(fn, inst.target.block, std::move(trueState), steps + 1, inlineDepth);
                    }
                    if (!trueOut.supported) return trueOut;

                    state.heap.unifyBranchAlias(heapTable_, inst.operands[0], condNode, false);
                    applyEdgeArgsInPlace(fn, inst.elseTarget, state);
                    SymEvalOutcome falseOut;
                    if (!checkBackedgeLimit(blockId, inst.elseTarget.block, state)) {
                        falseOut = {true, "loop unroll bound reached", false, zero64_, {}, {}, zero1_, one1_};
                    } else {
                        falseOut = evalBlockPath(fn, inst.elseTarget.block, std::move(state), steps + 1, inlineDepth);
                    }
                    if (!falseOut.supported) return falseOut;

                    BVNodeId tExc = (trueOut.exceededBoundNode != 0) ? trueOut.exceededBoundNode : zero1_;
                    BVNodeId fExc = (falseOut.exceededBoundNode != 0) ? falseOut.exceededBoundNode : zero1_;
                    if (trueOut.hasReturn && falseOut.hasReturn) {
                        BVNodeId tThrew = (trueOut.threwNode != 0) ? trueOut.threwNode : zero1_;
                        BVNodeId fThrew = (falseOut.threwNode != 0) ? falseOut.threwNode : zero1_;
                        BVNodeId merged = (tThrew == one1_ && fThrew != one1_) ? falseOut.returnVal
                                        : (fThrew == one1_ && tThrew != one1_) ? trueOut.returnVal
                                        : ctx_.make_ite(condNode, trueOut.returnVal, falseOut.returnVal);
                        BVNodeId mergedThrew = (tThrew == fThrew) ? tThrew : ctx_.make_ite(condNode, tThrew, fThrew);
                        BVNodeId mergedExc = (tExc == fExc) ? tExc : ctx_.make_ite(condNode, tExc, fExc);
                        SymbolicHeapState mergedHeap = SymbolicHeapState::mergeBranches(
                            heapTable_, condNode, trueOut.finalHeap, falseOut.finalHeap,
                            trueOut.returnVal, tThrew, falseOut.returnVal, fThrew, merged);
                        std::string retObj = (trueOut.returnObjPath == falseOut.returnObjPath) ? trueOut.returnObjPath
                                           : (tThrew == one1_ && fThrew != one1_) ? falseOut.returnObjPath
                                           : (fThrew == one1_ && tThrew != one1_) ? trueOut.returnObjPath : std::string{};
                        uint32_t retCtor = (trueOut.returnCtorOf == falseOut.returnCtorOf) ? trueOut.returnCtorOf
                                         : (tThrew == one1_ && fThrew != one1_) ? falseOut.returnCtorOf
                                         : (fThrew == one1_ && tThrew != one1_) ? trueOut.returnCtorOf : UINT32_MAX;
                        BVNodeId tKind = returnKindOf(trueOut);
                        BVNodeId fKind = returnKindOf(falseOut);
                        BVNodeId mergedKind = (tThrew == one1_ && fThrew != one1_) ? fKind
                                            : (fThrew == one1_ && tThrew != one1_) ? tKind
                                            : mergeKinds(condNode, tKind, fKind);
                        return {true, {}, true, merged, std::move(mergedHeap), std::move(retObj), mergedThrew, mergedExc,
                                UINT32_MAX, 0, retCtor, {}, {}, mergedKind};
                    }
                    if (trueOut.hasReturn) {
                        trueOut.exceededBoundNode = ctx_.make_binary(BVOp::Or, ctx_.make_unary(BVOp::Not, condNode), tExc);
                        return trueOut;
                    }
                    if (falseOut.hasReturn) {
                        falseOut.exceededBoundNode = ctx_.make_binary(BVOp::Or, condNode, fExc);
                        return falseOut;
                    }
                    return {true, "loop unroll bound reached", false, zero64_, {}, {}, zero1_, one1_};
                }
                case Op::Ret: {
                    if (inModuleInit_) saveModuleInitSnapshot(state);
                    BVNodeId ret = inst.operands.empty() ? zero64_ : toBV64(getVal(state, inst.operands[0]));
                    std::string retPath = inst.operands.empty() ? std::string{} : state.heap.lookupObjPath(inst.operands[0]);
                    uint32_t retFnRef = (!inst.operands.empty() && state.funcRefs.count(inst.operands[0]))
                                            ? state.funcRefs[inst.operands[0]] : UINT32_MAX;
                    uint32_t retFnEnv = (!inst.operands.empty() && state.funcEnvs.count(inst.operands[0]))
                                            ? state.funcEnvs[inst.operands[0]] : 0;
                    uint32_t retCtor = (!inst.operands.empty() && state.ctorOf.count(inst.operands[0]))
                                           ? state.ctorOf[inst.operands[0]] : UINT32_MAX;
                    std::string retGlob = (!inst.operands.empty() && state.globalRefs.count(inst.operands[0]))
                                              ? state.globalRefs[inst.operands[0]] : std::string{};
                    std::vector<BVNodeId> retArr = (!inst.operands.empty() && state.arrayValues.count(inst.operands[0]))
                                                       ? state.arrayValues[inst.operands[0]] : std::vector<BVNodeId>{};
                    if (inlineDepth == 0 && !inModuleInit_ && !inst.operands.empty()) {
                        canonicalizeSeqReturn(inst.operands[0], state, retPath, ret);
                    }
                    BVNodeId pathThrew = (state.threwCond != 0) ? state.threwCond : zero1_;
                    BVNodeId pathExc = (state.exceededCond != 0) ? state.exceededCond : zero1_;
                    BVNodeId retKind = (inst.operands.empty() || state.undefValues.count(inst.operands[0]))
                                           ? kindConst(kKindNullish) : kindOf(state, inst.operands[0]);
                    return {true, {}, true, ret, std::move(state.heap), std::move(retPath), pathThrew, pathExc,
                            retFnRef, retFnEnv, retCtor, std::move(retGlob), std::move(retArr), retKind};
                }
                default:
                    if (inModuleInit_) {
                        if (inst.result != il::kNoValue) {
                            state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
                        }
                        break;
                    }
                    return {false, std::string("unsupported IL operation: ") + il::opName(inst.op), false, zero64_, {}, {}, zero1_, zero1_};
            }
            if (inst.result != il::kNoValue && inst.type == il::Type::Bool) setKind(state, inst.result, kindConst(kKindBool));
            if (jumpedInPlace) break;
        }
        if (!jumpedInPlace) {
            return {false, "block terminated without Ret/Jump/Branch", false, zero64_, {}, {}, zero1_, zero1_};
        }
    }
}

}  // namespace bronze::check
