#include "check/prove_equiv.h"
#include "check/prove_equiv_heap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <verify/bv_solver.hpp>

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

constexpr uint32_t kMaxBlockSteps = 120;
constexpr uint32_t kMaxTotalBlockVisits = 1536;
constexpr uint32_t kMaxInlineDepth = 6;
constexpr uint32_t kMaxModuleInitSteps = 12000;
constexpr uint32_t kMaxModuleInitVisits = 24000;
constexpr uint64_t kThrowSentinelImm = 0xDEAD0000DEAD0001ULL;

struct SymState {
    std::unordered_map<il::ValueId, BVNodeId> values;
    std::unordered_map<uint64_t, BVNodeId> envSlots;
    std::unordered_map<uint32_t, std::string> globalRefs;
    std::unordered_map<uint64_t, std::string> envGlobalRefs;
    std::unordered_map<il::ValueId, std::vector<BVNodeId>> arrayValues;
    std::unordered_map<uint64_t, std::vector<BVNodeId>> envArrays;
    std::unordered_map<il::ValueId, uint32_t> envIds;
    std::unordered_map<uint32_t, uint32_t> envParent;
    std::unordered_map<il::ValueId, uint32_t> funcRefs;
    std::unordered_map<il::ValueId, uint32_t> funcEnvs;
    std::unordered_map<uint64_t, uint32_t> envFuncRefs;
    std::unordered_map<uint64_t, uint32_t> envFuncEnvs;
    std::unordered_set<il::ValueId> undefValues;
    std::unordered_set<uint64_t> envUndef;
    uint32_t nextEnvId = 1;
    SymbolicHeapState heap;
};

struct SymEvalOutcome {
    bool supported = true;
    std::string unsupportedReason;
    bool hasReturn = false;
    BVNodeId returnVal = 0;
    SymbolicHeapState finalHeap;
    std::string returnObjPath;
};

class SymbolicIlBuilder {
public:
    SymbolicIlBuilder(BVContext& ctx,
                      const il::Module& mod,
                      bool strictJs32Bitwise = false,
                      SymbolicHeapTable* sharedHeapTable = nullptr)
        : ctx_(ctx),
          mod_(mod),
          strictJs32Bitwise_(strictJs32Bitwise),
          ownedHeapTable_(ctx),
          heapTable_(sharedHeapTable ? *sharedHeapTable : ownedHeapTable_) {
        zero64_ = ctx_.make_const(0, 64);
        one64_ = ctx_.make_const(1, 64);
        allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
        throwSentinel64_ = ctx_.make_const(kThrowSentinelImm, 64);
        initModuleState();
    }

    SymEvalOutcome evalFunction(const il::Function& fn,
                                const std::vector<BVNodeId>& argNodes,
                                uint32_t inlineDepth = 0,
                                const SymbolicHeapState* callerHeap = nullptr,
                                const std::string& thisPath = "this",
                                const std::vector<std::string>* callerArgPaths = nullptr,
                                uint32_t callerEnvId = 0,
                                const std::vector<bool>* callerUndefArgs = nullptr) {
        if (fn.blocks.empty() || fn.isExternal) {
            return {false, "external or empty function", false, zero64_, {}, {}};
        }
        if (inlineDepth > kMaxInlineDepth) {
            return {false, "call recursion depth exceeded", false, zero64_, {}, {}};
        }

        SymState initState = moduleState_;
        if (callerHeap) {
            initState.heap.writes = callerHeap->writes;
            initState.heap.propFuncRefs = callerHeap->propFuncRefs;
            initState.heap.localObjCounter = callerHeap->localObjCounter;
        }
        uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
        for (uint32_t i = 0; i < firstSrc; ++i) initState.values[i] = zero64_;
        if (fn.needsEnv) initState.envIds[0] = callerEnvId;
        uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
        for (uint32_t i = 0; i < srcCount; ++i) {
            initState.values[firstSrc + i] = (i < argNodes.size()) ? argNodes[i] : zero64_;
            if ((callerUndefArgs && i < callerUndefArgs->size() && (*callerUndefArgs)[i]) || i >= argNodes.size()) {
                initState.undefValues.insert(firstSrc + i);
            }
        }
        initState.heap.initFunctionHeap(heapTable_, fn, initState.values, thisPath, callerArgPaths);
        return evalBlockPath(fn, 0, initState, 0, inlineDepth);
    }

private:
    BVContext& ctx_;
    const il::Module& mod_;
    bool strictJs32Bitwise_ = false;
    SymbolicHeapTable ownedHeapTable_;
    SymbolicHeapTable& heapTable_;
    BVNodeId zero64_ = 0;
    BVNodeId one64_ = 0;
    BVNodeId allOnes64_ = 0;
    BVNodeId throwSentinel64_ = 0;
    uint32_t totalVisits_ = 0;
    bool hitLoopUnrollBound_ = false;
    bool inModuleInit_ = false;
    SymState moduleState_;

public:
    bool hitLoopUnrollBound() const { return hitLoopUnrollBound_; }

private:
    void initModuleState() {
        const il::Function* entryFn = nullptr;
        for (const auto& f : mod_.functions) {
            if (!f.isExternal && (f.isEntryPoint || f.name == "main")) {
                entryFn = &f;
                break;
            }
        }
        if (!entryFn || entryFn->blocks.empty()) return;
        inModuleInit_ = true;
        uint32_t savedVisits = totalVisits_;
        bool savedHit = hitLoopUnrollBound_;
        SymState initSt;
        uint32_t firstSrc = static_cast<uint32_t>(entryFn->firstSourceParam());
        for (uint32_t i = 0; i < firstSrc; ++i) initSt.values[i] = zero64_;
        initSt.heap.initFunctionHeap(heapTable_, *entryFn, initSt.values, "this", nullptr);
        auto out = evalBlockPath(*entryFn, 0, std::move(initSt), 0, 0);
        if (out.supported) {
            moduleState_.heap.writes = std::move(out.finalHeap.writes);
            moduleState_.heap.propFuncRefs = std::move(out.finalHeap.propFuncRefs);
            moduleState_.heap.localObjCounter = out.finalHeap.localObjCounter;
        }
        inModuleInit_ = false;
        totalVisits_ = savedVisits;
        hitLoopUnrollBound_ = savedHit;
    }

    uint64_t resolveEnvSlotKey(const SymState& st, il::ValueId envVal, uint32_t depth, uint32_t index) const {
        uint32_t cur = 0;
        auto it = st.envIds.find(envVal);
        if (it != st.envIds.end()) cur = it->second;
        for (uint32_t d = 0; d < depth; ++d) {
            auto pIt = st.envParent.find(cur);
            if (pIt != st.envParent.end()) cur = pIt->second;
            else cur = 0;
        }
        return (static_cast<uint64_t>(cur) << 32) | index;
    }

    BVNodeId getVal(const SymState& st, il::ValueId v) const {
        auto it = st.values.find(v);
        return (it != st.values.end()) ? it->second : zero64_;
    }
    BVNodeId toBool1(BVNodeId node) {
        return (ctx_.width(node) == 1) ? node : ctx_.make_binary(BVOp::Ne, toBV64(node), zero64_);
    }
    BVNodeId toBV64(BVNodeId node) {
        uint32_t w = ctx_.width(node);
        return (w >= 64) ? node : ((w == 1) ? ctx_.make_zext(node, 64 - w) : ctx_.make_sext(node, 64 - w));
    }
    BVNodeId toSInt32In64(BVNodeId node) {
        return ctx_.make_sext(ctx_.make_extract(toBV64(node), 31, 0), 32);
    }

    void applyEdgeArgsInPlace(const il::Function& fn, const il::BlockTarget& edge, SymState& state) {
        if (edge.block >= fn.blocks.size()) return;
        const auto& targetBlk = fn.blocks[edge.block];
        std::vector<std::pair<il::ValueId, BVNodeId>> updates;
        std::vector<std::pair<il::ValueId, bool>> undefUpdates;
        updates.reserve(targetBlk.params.size());
        for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
            il::ValueId src = edge.args[k], dst = targetBlk.params[k].id;
            updates.emplace_back(dst, toBV64(getVal(state, src)));
            undefUpdates.emplace_back(dst, state.undefValues.count(src) > 0);
            state.heap.propagateAlias(src, dst);
            if (state.funcRefs.count(src)) state.funcRefs[dst] = state.funcRefs[src];
            if (state.funcEnvs.count(src)) state.funcEnvs[dst] = state.funcEnvs[src];
            if (state.envIds.count(src)) state.envIds[dst] = state.envIds[src];
            if (state.arrayValues.count(src)) state.arrayValues[dst] = state.arrayValues[src];
        }
        for (const auto& [dst, val] : updates) state.values[dst] = val;
        for (const auto& [dst, isU] : undefUpdates) {
            if (isU) state.undefValues.insert(dst);
            else state.undefValues.erase(dst);
        }
    }

    void saveModuleInitSnapshot(const SymState& state) {
        if (!inModuleInit_) return;
        moduleState_.envSlots = state.envSlots;
        moduleState_.envGlobalRefs = state.envGlobalRefs;
        moduleState_.envArrays = state.envArrays;
        moduleState_.envParent = state.envParent;
        moduleState_.envFuncRefs = state.envFuncRefs;
        moduleState_.envFuncEnvs = state.envFuncEnvs;
        moduleState_.envUndef = state.envUndef;
        moduleState_.nextEnvId = state.nextEnvId;
        moduleState_.heap.envObjPaths = state.heap.envObjPaths;
        moduleState_.heap.writes = state.heap.writes;
        moduleState_.heap.propFuncRefs = state.heap.propFuncRefs;
        moduleState_.heap.localObjCounter = state.heap.localObjCounter;
    }

    SymEvalOutcome invokeCallee(const il::Function& callee,
                                const il::Instruction& inst,
                                size_t startOp,
                                const std::string& thisPath,
                                uint32_t envId,
                                uint32_t inlineDepth,
                                SymState& state,
                                bool writeScalarRet = true) {
        std::vector<BVNodeId> callArgs;
        std::vector<std::string> callPaths;
        std::vector<bool> callUndef;
        for (size_t k = startOp; k < inst.operands.size(); ++k) {
            callArgs.push_back(toBV64(getVal(state, inst.operands[k])));
            callPaths.push_back(state.heap.lookupObjPath(inst.operands[k]));
            callUndef.push_back(state.undefValues.count(inst.operands[k]) > 0);
        }
        auto sub = evalFunction(callee, callArgs, inlineDepth + 1, &state.heap,
                                thisPath.empty() ? "this" : thisPath, &callPaths, envId, &callUndef);
        if (!sub.supported || !sub.hasReturn) {
            return {false, sub.unsupportedReason.empty() ? "callee hit loop unroll bound" : sub.unsupportedReason,
                    false, zero64_, {}, {}};
        }
        state.heap.writes = std::move(sub.finalHeap.writes);
        state.heap.propFuncRefs = std::move(sub.finalHeap.propFuncRefs);
        state.heap.localObjCounter = sub.finalHeap.localObjCounter;
        if (writeScalarRet) state.values[inst.result] = sub.returnVal;
        if (!sub.returnObjPath.empty() && inst.result != il::kNoValue) {
            state.heap.objPaths[inst.result] = sub.returnObjPath;
            state.values[inst.result] = sub.returnVal;
        }
        return {true, {}, false, zero64_, {}, {}};
    }

    bool tryEvalGlobalCall(const std::string& gName,
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

    SymEvalOutcome evalBlockPath(const il::Function& fn,
                                 il::BlockId blockId,
                                 SymState state,
                                 uint32_t steps,
                                 uint32_t inlineDepth) {
        const uint32_t maxSteps = inModuleInit_ ? kMaxModuleInitSteps : kMaxBlockSteps;
        const uint32_t maxVisits = inModuleInit_ ? kMaxModuleInitVisits : kMaxTotalBlockVisits;
        while (true) {
            if (++totalVisits_ > maxVisits) {
                if (inModuleInit_) {
                    saveModuleInitSnapshot(state);
                    return {true, {}, true, zero64_, std::move(state.heap), {}};
                }
                return {false, "symbolic path limit exceeded", false, zero64_};
            }
            if (steps > maxSteps) {
                if (inModuleInit_) {
                    saveModuleInitSnapshot(state);
                    return {true, {}, true, zero64_, std::move(state.heap), {}};
                }
                hitLoopUnrollBound_ = true;
                return {true, "loop unroll bound reached", false, zero64_};
            }
            if (blockId >= fn.blocks.size()) {
                return {false, "invalid block target", false, zero64_};
            }

            bool jumpedInPlace = false;
            const il::Block& blk = fn.blocks[blockId];
            for (size_t instIdx = 0; instIdx < blk.instructions.size(); ++instIdx) {
            const auto& inst = blk.instructions[instIdx];
            using Op = il::Op;
            switch (inst.op) {
                case Op::ConstI32:
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64);
                    break;
                case Op::ConstF64: {
                    if (std::isnan(inst.immF64) || std::isinf(inst.immF64) || std::trunc(inst.immF64) != inst.immF64) {
                        if (inModuleInit_) { state.values[inst.result] = zero64_; break; }
                        return {false, std::isnan(inst.immF64) || std::isinf(inst.immF64)
                                           ? "non-finite floating-point constant"
                                           : "fractional floating-point constant", false, zero64_};
                    }
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immF64)), 64);
                    break;
                }
                case Op::ConstBool:
                    state.values[inst.result] = (inst.immI32 != 0) ? one64_ : zero64_;
                    break;
                case Op::ConstNull:
                case Op::ConstUndefined:
                    state.values[inst.result] = zero64_;
                    state.undefValues.insert(inst.result);
                    break;
                case Op::IsNullish:
                    state.values[inst.result] = (!inst.operands.empty() && state.undefValues.count(inst.operands[0])) ? one64_ : zero64_;
                    break;
                case Op::ToInt32:
                    state.values[inst.result] = toSInt32In64(inst.operands.empty() ? zero64_ : getVal(state, inst.operands[0]));
                    break;
                case Op::Box:
                case Op::Unbox:
                case Op::ToNumeric:
                case Op::ToStr:
                case Op::NumTruthy: {
                    if (!inst.operands.empty()) {
                        il::ValueId src = inst.operands[0];
                        state.values[inst.result] = toBV64(getVal(state, src));
                        state.heap.propagateAlias(src, inst.result);
                        if (state.arrayValues.count(src)) state.arrayValues[inst.result] = state.arrayValues[src];
                        if (state.funcRefs.count(src)) state.funcRefs[inst.result] = state.funcRefs[src];
                        if (state.funcEnvs.count(src)) state.funcEnvs[inst.result] = state.funcEnvs[src];
                        if (state.globalRefs.count(src)) state.globalRefs[inst.result] = state.globalRefs[src];
                        if (state.envIds.count(src)) state.envIds[inst.result] = state.envIds[src];
                        if (state.undefValues.count(src)) state.undefValues.insert(inst.result);
                    } else if (inst.op == Op::Box && inst.keyIndex < mod_.keyConstants.size()) {
                        const std::string& lit = mod_.keyConstants[inst.keyIndex];
                        state.values[inst.result] = ctx_.make_const(hashStringLiteral(lit), 64);
                        std::vector<BVNodeId> chars;
                        for (unsigned char c : lit) chars.push_back(ctx_.make_const(c, 64));
                        state.arrayValues[inst.result] = std::move(chars);
                    } else {
                        state.values[inst.result] = zero64_;
                    }
                    break;
                }
                case Op::TypeOf: {
                    std::string tag = "number";
                    if (!inst.operands.empty()) {
                        if (state.undefValues.count(inst.operands[0])) tag = "undefined";
                        else if (state.funcRefs.count(inst.operands[0])) tag = "function";
                    }
                    state.values[inst.result] = ctx_.make_const(hashStringLiteral(tag), 64);
                    break;
                }
                case Op::InstanceOf:
                case Op::ClosureIs:
                case Op::GetNewTarget:
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
                    state.funcRefs[inst.result] = inst.calleeIndex;
                    state.funcEnvs[inst.result] = (inst.op == Op::CreateFunction && !inst.operands.empty() &&
                                                   state.envIds.count(inst.operands[0]))
                                                      ? state.envIds[inst.operands[0]] : 0;
                    std::string fnPath = "__fn_" + std::to_string(inst.calleeIndex);
                    state.heap.objPaths[inst.result] = fnPath;
                    state.values[inst.result] = heapTable_.getObjectToken(fnPath);
                    break;
                }
                case Op::MethodDef: {
                    if (inst.operands.size() >= 2 && inst.keyIndex < mod_.keyConstants.size()) {
                        std::string rPath = state.heap.lookupObjPath(inst.operands[0]);
                        if (!rPath.empty() && state.funcRefs.count(inst.operands[1])) {
                            state.heap.propFuncRefs[rPath + "." + mod_.keyConstants[inst.keyIndex]] =
                                state.funcRefs[inst.operands[1]];
                        }
                    }
                    break;
                }
                case Op::PropGet: {
                    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "malformed PropGet", false, zero64_, {}, {}};
                    }
                    const std::string& key = mod_.keyConstants[inst.keyIndex];
                    auto gIt = state.globalRefs.find(inst.operands[0]);
                    if (gIt != state.globalRefs.end()) {
                        state.globalRefs[inst.result] = gIt->second + "." + key;
                        state.values[inst.result] = one64_;
                        break;
                    }
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && key == "length") {
                        state.values[inst.result] = ctx_.make_const(arrIt->second.size(), 64);
                        break;
                    }
                    std::string rPath = state.heap.lookupObjPath(inst.operands[0]);
                    if (!rPath.empty() && state.heap.propFuncRefs.count(rPath + "." + key)) {
                        state.funcRefs[inst.result] = state.heap.propFuncRefs[rPath + "." + key];
                    }
                    BVNodeId propVal = zero64_;
                    bool isUndefProp = false;
                    if (!state.heap.evalPropGet(heapTable_, inst.operands[0], key, inst.result, propVal, &isUndefProp)) {
                        if (inModuleInit_) { state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result); break; }
                        return {false, "unsupported PropGet on non-tracked receiver", false, zero64_, {}, {}};
                    }
                    if (isUndefProp) state.undefValues.insert(inst.result);
                    state.values[inst.result] = propVal;
                    break;
                }
                case Op::PropSet:
                case Op::DefineOwnAttr: {
                    if (inst.operands.size() < 2 || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "malformed PropSet", false, zero64_, {}, {}};
                    }
                    const std::string& key = mod_.keyConstants[inst.keyIndex];
                    BVNodeId valNode = toBV64(getVal(state, inst.operands[1]));
                    std::string rPath = state.heap.lookupObjPath(inst.operands[0]);
                    if (!rPath.empty() && state.funcRefs.count(inst.operands[1])) {
                        state.heap.propFuncRefs[rPath + "." + key] = state.funcRefs[inst.operands[1]];
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
                        return {false, "unsupported PropSet on non-tracked receiver", false, zero64_, {}, {}};
                    }
                    if (inst.result != il::kNoValue) state.values[inst.result] = valNode;
                    break;
                }
                case Op::NumericStep:
                    state.values[inst.result] = ctx_.make_binary(
                        BVOp::Add, toBV64(getVal(state, inst.operands[0])),
                        ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64));
                    break;
                case Op::Add:
                case Op::Sub:
                case Op::Mul: {
                    BVOp bvOp = (inst.op == Op::Add) ? BVOp::Add : (inst.op == Op::Sub ? BVOp::Sub : BVOp::Mul);
                    state.values[inst.result] = ctx_.make_binary(
                        bvOp, toBV64(getVal(state, inst.operands[0])), toBV64(getVal(state, inst.operands[1])));
                    break;
                }
                case Op::Pow: {
                    bool ok = false;
                    BVNodeId r = evalPowSymbolic(ctx_, toBV64(getVal(state, inst.operands[0])),
                                                 toBV64(getVal(state, inst.operands[1])), ok);
                    if (!ok) return {false, "unsupported symbolic exponentiation", false, zero64_};
                    state.values[inst.result] = r;
                    break;
                }
                case Op::Div:
                case Op::Mod: {
                    BVNodeId divVal = zero64_;
                    std::string divErr;
                    if (!evalDivOrModSymbolic(ctx_, mod_, blk, instIdx, toBV64(getVal(state, inst.operands[0])),
                                              toBV64(getVal(state, inst.operands[1])), state.heap, divVal, &divErr)) {
                        return {false, divErr, false, zero64_, {}, {}};
                    }
                    state.values[inst.result] = divVal;
                    break;
                }
                case Op::Neg:
                    state.values[inst.result] = ctx_.make_unary(BVOp::Neg, toBV64(getVal(state, inst.operands[0])));
                    break;
                case Op::MathImul:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVOp bvOp = (inst.op == Op::MathImul) ? BVOp::Mul
                                : (inst.op == Op::BitAnd) ? BVOp::And
                                : (inst.op == Op::BitOr)  ? BVOp::Or : BVOp::Xor;
                    state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(bvOp, a32, b32), 32);
                    break;
                }
                case Op::BitNot: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    state.values[inst.result] = ctx_.make_sext(ctx_.make_unary(BVOp::Not, a32), 32);
                    break;
                }
                case Op::Shl:
                case Op::Shr:
                case Op::UShr: {
                    BVNodeId a32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[0])), 31, 0);
                    BVNodeId b32 = ctx_.make_extract(toBV64(getVal(state, inst.operands[1])), 31, 0);
                    BVNodeId sh32 = ctx_.make_binary(BVOp::And, b32, ctx_.make_const(31, 32));
                    BVOp shOp = (inst.op == Op::Shl) ? BVOp::Shl
                                                     : (inst.op == Op::Shr ? BVOp::AShr : BVOp::LShr);
                    BVNodeId r32 = ctx_.make_binary(shOp, a32, sh32);
                    state.values[inst.result] = (inst.op == Op::UShr)
                                                    ? ctx_.make_zext(r32, 32)
                                                    : ctx_.make_sext(r32, 32);
                    break;
                }
                case Op::CmpEq:
                case Op::StrictEq:
                case Op::LooseEq:
                case Op::CmpNe: {
                    bool isEq = (inst.op != Op::CmpNe);
                    bool u0 = state.undefValues.count(inst.operands[0]) > 0;
                    bool u1 = state.undefValues.count(inst.operands[1]) > 0;
                    if (u0 != u1) { state.values[inst.result] = isEq ? zero64_ : one64_; break; }
                    if (u0 && u1) { state.values[inst.result] = isEq ? one64_ : zero64_; break; }
                    BVNodeId a = toBV64(getVal(state, inst.operands[0])), b = toBV64(getVal(state, inst.operands[1]));
                    state.values[inst.result] = toBV64(ctx_.make_binary(isEq ? BVOp::Eq : BVOp::Ne, a, b));
                    state.heap.recordObjComparison(inst.result, inst.operands[0], inst.operands[1], isEq);
                    break;
                }
                case Op::CmpLt: case Op::RelLt: case Op::CmpLe: case Op::RelLe:
                case Op::CmpGt: case Op::RelGt: case Op::CmpGe: case Op::RelGe: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0])), b = toBV64(getVal(state, inst.operands[1]));
                    bool swapOps = (inst.op == Op::CmpGt || inst.op == Op::RelGt || inst.op == Op::CmpGe || inst.op == Op::RelGe);
                    bool isLe = (inst.op == Op::CmpLe || inst.op == Op::RelLe || inst.op == Op::CmpGe || inst.op == Op::RelGe);
                    state.values[inst.result] = toBV64(ctx_.make_binary(isLe ? BVOp::Sle : BVOp::Slt, swapOps ? b : a, swapOps ? a : b));
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
                case Op::ElemGet:
                case Op::ElemGetTyped: {
                    if (inst.operands.size() < 2) return {false, "malformed ElemGet", false, zero64_, {}, {}};
                    BVNodeId idx = toBV64(getVal(state, inst.operands[1]));
                    auto arrIt = state.arrayValues.find(inst.operands[0]);
                    if (arrIt != state.arrayValues.end() && !arrIt->second.empty()) {
                        const auto& elems = arrIt->second;
                        if (idx < ctx_.nodes().size() && ctx_.nodes()[idx].op == BVOp::Const) {
                            int64_t cIdx = static_cast<int64_t>(ctx_.nodes()[idx].imm);
                            state.values[inst.result] = (cIdx >= 0 && static_cast<size_t>(cIdx) < elems.size())
                                                            ? elems[static_cast<size_t>(cIdx)] : zero64_;
                            if (cIdx < 0 || static_cast<size_t>(cIdx) >= elems.size()) state.undefValues.insert(inst.result);
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
                        return {false, "unsupported dynamic array ElemGet", false, zero64_, {}, {}};
                    }
                    if (isUndefElem) state.undefValues.insert(inst.result);
                    state.values[inst.result] = elemVal;
                    break;
                }
                case Op::ElemSet:
                case Op::ElemSetTyped: {
                    if (inst.operands.size() < 3) return {false, "malformed ElemSet", false, zero64_, {}, {}};
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
                        return {false, "unsupported ElemSet on non-tracked receiver", false, zero64_, {}, {}};
                    }
                    if (inst.result != il::kNoValue) state.values[inst.result] = valNode;
                    break;
                }
                case Op::MathUnary: {
                    BVNodeId a = toBV64(getVal(state, inst.operands[0]));
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if (kind == il::MathUnaryFn::Abs) {
                        state.values[inst.result] = ctx_.make_ite(ctx_.make_binary(BVOp::Slt, a, zero64_),
                                                                  ctx_.make_unary(BVOp::Neg, a), a);
                    } else if (kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil) {
                        state.values[inst.result] = evalFloorOrCeilSymbolic(
                            ctx_, state.heap, inst.operands[0], a, kind == il::MathUnaryFn::Ceil);
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
                        return {false, "unsupported transcendental MathUnary", false, zero64_, {}, {}};
                    }
                    break;
                }
                case Op::GlobalGet:
                case Op::ResolveName: {
                    std::string gName;
                    if (inst.keyIndex < mod_.keyConstants.size()) {
                        gName = mod_.keyConstants[inst.keyIndex];
                        state.globalRefs[inst.result] = gName;
                    }
                    if (gName == "undefined" || gName == "NaN") {
                        state.values[inst.result] = zero64_;
                        state.undefValues.insert(inst.result);
                        break;
                    }
                    std::string gPath = "__fn_" + (gName.empty() ? std::string("global") : gName);
                    for (size_t fi = 0; fi < mod_.functions.size(); ++fi) {
                        const auto& cand = mod_.functions[fi];
                        if (!cand.isExternal && !cand.isEntryPoint &&
                            (cand.name == gName || cand.displayName == gName)) {
                            state.funcRefs[inst.result] = static_cast<uint32_t>(fi);
                            gPath = "__fn_" + std::to_string(fi);
                            break;
                        }
                    }
                    state.heap.objPaths[inst.result] = gPath;
                    state.values[inst.result] = heapTable_.getObjectToken(gPath);
                    break;
                }
                case Op::Construct: {
                    state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
                    std::string localPath = state.heap.lookupObjPath(inst.result);
                    if (!inst.operands.empty() && state.funcRefs.count(inst.operands[0])) {
                        uint32_t ctorIdx = state.funcRefs[inst.operands[0]];
                        uint32_t ctorEnv = state.funcEnvs.count(inst.operands[0]) ? state.funcEnvs[inst.operands[0]] : 0;
                        if (ctorIdx < mod_.functions.size() && !mod_.functions[ctorIdx].isExternal) {
                            auto sub = invokeCallee(mod_.functions[ctorIdx], inst, 1, localPath,
                                                    ctorEnv, inlineDepth, state, false);
                            if (!sub.supported) return sub;
                            break;
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
                    if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) {
                        return {false, "unsupported dynamic method call", false, zero64_, {}, {}};
                    }
                    const std::string& method = mod_.keyConstants[inst.keyIndex];
                    if (state.globalRefs.count(inst.operands[0])) {
                        std::string gErr;
                        if (tryEvalGlobalCall(state.globalRefs[inst.operands[0]], method, inst, state, gErr)) {
                            if (!gErr.empty()) return {false, gErr, false, zero64_, {}, {}};
                            break;
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
                            break;
                        }
                        BVNodeId chVal = zero64_;
                        if (state.heap.evalElemGet(heapTable_, inst.operands[0], idx, inst.result, chVal)) {
                            state.values[inst.result] = ctx_.make_binary(BVOp::And, chVal, ctx_.make_const(0xFFFF, 64));
                            break;
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
                            break;
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
                            break;
                        }
                    }
                    if ((method == "slice" || method == "subarray") && !inst.operands.empty()) {
                        state.values[inst.result] = toBV64(getVal(state, inst.operands[0]));
                        state.heap.propagateAlias(inst.operands[0], inst.result);
                        break;
                    }
                    std::string recvPath = state.heap.lookupObjPath(inst.operands[0]);
                    const il::Function* callee = (inst.directTarget != il::Instruction::kNoDirectTarget &&
                                                  inst.directTarget < mod_.functions.size())
                                                     ? &mod_.functions[inst.directTarget]
                                                     : findMethodCalleeInModule(mod_, state.heap, recvPath, method);
                    if (callee && !callee->isExternal) {
                        auto sub = invokeCallee(*callee, inst, 1, recvPath, 0, inlineDepth, state);
                        if (!sub.supported) return sub;
                        break;
                    }
                    if (inModuleInit_) { state.values[inst.result] = zero64_; break; }
                    return {false, "unsupported method call: " + method, false, zero64_, {}, {}};
                }
                case Op::DynamicCall: {
                    if (!inst.operands.empty() && state.globalRefs.count(inst.operands[0])) {
                        std::string gErr;
                        if (tryEvalGlobalCall(state.globalRefs[inst.operands[0]], "", inst, state, gErr)) {
                            if (!gErr.empty()) return {false, gErr, false, zero64_, {}, {}};
                            break;
                        }
                    }
                    if (inst.operands.empty() || !state.funcRefs.count(inst.operands[0])) {
                        if (inModuleInit_) { state.values[inst.result] = zero64_; break; }
                        return {false, "unsupported IL operation: call.dynamic", false, zero64_, {}, {}};
                    }
                    uint32_t calleeIdx = state.funcRefs[inst.operands[0]];
                    if (calleeIdx >= mod_.functions.size() || mod_.functions[calleeIdx].isExternal) {
                        return {false, "external dynamic callee", false, zero64_, {}, {}};
                    }
                    uint32_t capEnv = state.funcEnvs.count(inst.operands[0]) ? state.funcEnvs[inst.operands[0]] : 0;
                    std::string thisPath = (inst.operands.size() > 1) ? state.heap.lookupObjPath(inst.operands[1]) : std::string{};
                    auto sub = invokeCallee(mod_.functions[calleeIdx], inst, 2, thisPath, capEnv, inlineDepth, state);
                    if (!sub.supported) return sub;
                    break;
                }
                case Op::Call: {
                    if (inst.calleeIndex >= mod_.functions.size()) {
                        return {false, "invalid callee index", false, zero64_, {}, {}};
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
                    auto sub = invokeCallee(callee, inst, startOp, thisPath, calleeEnvId, inlineDepth, state);
                    if (!sub.supported) return sub;
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
                    auto it = state.envSlots.find(key);
                    state.values[inst.result] = (it != state.envSlots.end()) ? it->second : zero64_;
                    state.heap.propagateEnvGet(key, inst.result);
                    if (state.envArrays.count(key)) state.arrayValues[inst.result] = state.envArrays[key];
                    if (state.envFuncRefs.count(key)) state.funcRefs[inst.result] = state.envFuncRefs[key];
                    if (state.envFuncEnvs.count(key)) state.funcEnvs[inst.result] = state.envFuncEnvs[key];
                    if (state.envGlobalRefs.count(key)) state.globalRefs[inst.result] = state.envGlobalRefs[key];
                    if (state.envUndef.count(key)) state.undefValues.insert(inst.result);
                    break;
                }
                case Op::Throw:
                    return {true, {}, true, throwSentinel64_, std::move(state.heap), {}};
                case Op::Jump:
                    applyEdgeArgsInPlace(fn, inst.target, state);
                    blockId = inst.target.block;
                    ++steps;
                    jumpedInPlace = true;
                    break;
                case Op::Branch: {
                    BVNodeId condNode = toBool1(toBV64(getVal(state, inst.operands[0])));
                    const auto& cNode = ctx_.nodes()[condNode];
                    if (cNode.op == BVOp::Const) {
                        const il::BlockTarget& edge = (cNode.imm != 0) ? inst.target : inst.elseTarget;
                        applyEdgeArgsInPlace(fn, edge, state);
                        blockId = edge.block;
                        ++steps;
                        jumpedInPlace = true;
                        break;
                    }
                    SymState trueState = state;
                    trueState.heap.unifyBranchAlias(heapTable_, inst.operands[0], condNode, true);
                    applyEdgeArgsInPlace(fn, inst.target, trueState);
                    auto trueOut = evalBlockPath(fn, inst.target.block, std::move(trueState), steps + 1, inlineDepth);
                    if (!trueOut.supported) return trueOut;

                    state.heap.unifyBranchAlias(heapTable_, inst.operands[0], condNode, false);
                    applyEdgeArgsInPlace(fn, inst.elseTarget, state);
                    auto falseOut = evalBlockPath(fn, inst.elseTarget.block, std::move(state), steps + 1, inlineDepth);
                    if (!falseOut.supported) return falseOut;

                    if (trueOut.hasReturn && falseOut.hasReturn) {
                        BVNodeId merged = ctx_.make_ite(condNode, trueOut.returnVal, falseOut.returnVal);
                        SymbolicHeapState mergedHeap = SymbolicHeapState::mergeBranches(
                            heapTable_, condNode, trueOut.finalHeap, falseOut.finalHeap);
                        std::string retObj = (trueOut.returnObjPath == falseOut.returnObjPath)
                                                 ? trueOut.returnObjPath : std::string{};
                        return {true, {}, true, merged, std::move(mergedHeap), std::move(retObj)};
                    }
                    if (trueOut.hasReturn) return trueOut;
                    if (falseOut.hasReturn) return falseOut;
                    return {true, "loop unroll bound reached", false, zero64_, {}, {}};
                }
                case Op::Ret: {
                    if (inModuleInit_) saveModuleInitSnapshot(state);
                    BVNodeId ret = inst.operands.empty() ? zero64_ : toBV64(getVal(state, inst.operands[0]));
                    std::string retPath = inst.operands.empty() ? std::string{} : state.heap.lookupObjPath(inst.operands[0]);
                    if (inlineDepth == 0 && !inModuleInit_ && !retPath.empty() && retPath.rfind("__local_", 0) == 0) {
                        std::string pref = retPath + ".";
                        std::vector<std::pair<std::string, BVNodeId>> retProps;
                        for (const auto& [k, v] : state.heap.writes) {
                            if (k.rfind(pref, 0) == 0) retProps.emplace_back("__ret." + k.substr(pref.size()), v);
                        }
                        for (const auto& [rk, rv] : retProps) state.heap.writes[rk] = rv;
                        retPath = "__ret";
                        ret = heapTable_.getObjectToken("__ret");
                    }
                    return {true, {}, true, ret, std::move(state.heap), std::move(retPath)};
                }
                default:
                    if (inModuleInit_) {
                        saveModuleInitSnapshot(state);
                        return {true, {}, true, zero64_, std::move(state.heap), {}};
                    }
                    return {false, std::string("unsupported IL operation: ") + il::opName(inst.op), false, zero64_, {}, {}};
            }
            if (jumpedInPlace) break;
        }
        if (!jumpedInPlace) {
            return {false, "block terminated without Ret/Jump/Branch", false, zero64_, {}, {}};
        }
        }
    }
};

}  // namespace

bool evalIlFunctionSymbolic(brass::verify::BVContext& ctx,
                            const il::Module& mod,
                            const il::Function& fn,
                            const std::vector<brass::verify::BVNodeId>& argNodes,
                            bool strictJs32Bitwise,
                            brass::verify::BVNodeId* outRetNode,
                            std::string* outReason,
                            SymbolicHeapTable* sharedHeapTable,
                            SymbolicHeapState* outFinalHeap,
                            bool* outHitLoopUnrollBound) {
    SymbolicIlBuilder builder(ctx, mod, strictJs32Bitwise, sharedHeapTable);
    auto out = builder.evalFunction(fn, argNodes);
    if (outHitLoopUnrollBound) *outHitLoopUnrollBound = builder.hitLoopUnrollBound();
    if (!out.supported || !out.hasReturn) {
        if (outReason) *outReason = out.unsupportedReason;
        return false;
    }
    if (outRetNode) *outRetNode = out.returnVal;
    if (outFinalHeap) *outFinalHeap = std::move(out.finalHeap);
    return true;
}

}  // namespace bronze::check
