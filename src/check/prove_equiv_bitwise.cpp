#include "check/prove_equiv_sym.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

namespace {

bool canReachNonEmpty(const il::Function& fn, il::BlockId start, il::BlockId goal, il::BlockId minBlock) {
    if (start >= fn.blocks.size() || goal >= fn.blocks.size()) return false;
    std::queue<il::BlockId> q;
    std::vector<uint8_t> vis(fn.blocks.size(), 0);
    auto pushEdge = [&](il::BlockId dst) {
        if (dst >= fn.blocks.size() || dst < minBlock) return false;
        if (dst == goal) return true;
        if (!vis[dst]) {
            vis[dst] = 1;
            q.push(dst);
        }
        return false;
    };
    const auto& sBlk = fn.blocks[start];
    if (!sBlk.instructions.empty()) {
        const auto& term = sBlk.instructions.back();
        if (term.op == il::Op::Jump && pushEdge(term.target.block)) return true;
        if (term.op == il::Op::Branch && (pushEdge(term.target.block) || pushEdge(term.elseTarget.block))) return true;
    }
    while (!q.empty()) {
        il::BlockId cur = q.front();
        q.pop();
        const auto& blk = fn.blocks[cur];
        if (blk.instructions.empty()) continue;
        const auto& term = blk.instructions.back();
        if (term.op == il::Op::Jump && pushEdge(term.target.block)) return true;
        if (term.op == il::Op::Branch && (pushEdge(term.target.block) || pushEdge(term.elseTarget.block))) return true;
    }
    return false;
}

uint32_t computeFunctionSymbolicLoopBound(const il::Function& fn, std::vector<uint8_t>& outInCycle) {
    outInCycle.assign(fn.blocks.size(), 0);
    std::vector<il::BlockId> headers;
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        if (canReachNonEmpty(fn, b, b, 0)) outInCycle[b] = 1;
        if (canReachNonEmpty(fn, b, b, b)) headers.push_back(b);
    }
    bool nested = false;
    for (size_t i = 0; i < headers.size() && !nested; ++i) {
        for (size_t j = i + 1; j < headers.size(); ++j) {
            if (canReachNonEmpty(fn, headers[i], headers[j], headers[i]) &&
                canReachNonEmpty(fn, headers[j], headers[i], headers[i])) {
                nested = true;
                break;
            }
        }
    }
    if (nested) return 3u;
    if (headers.size() >= 2) return 5u;
    return 8u;
}

}  // namespace

SymbolicIlBuilder::SymbolicIlBuilder(BVContext& ctx,
                                     const il::Module& mod,
                                     bool strictJs32Bitwise,
                                     SymbolicHeapTable* sharedHeapTable)
    : ctx_(ctx),
      mod_(mod),
      strictJs32Bitwise_(strictJs32Bitwise),
      ownedHeapTable_(ctx),
      heapTable_(sharedHeapTable ? *sharedHeapTable : ownedHeapTable_) {
    zero1_ = ctx_.make_const(0, 1);
    one1_ = ctx_.make_const(1, 1);
    zero64_ = ctx_.make_const(0, 64);
    one64_ = ctx_.make_const(1, 64);
    allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
    initModuleState();
}

SymEvalOutcome SymbolicIlBuilder::evalFunction(const il::Function& fn,
                                               const std::vector<BVNodeId>& argNodes,
                                               uint32_t inlineDepth,
                                               const SymbolicHeapState* callerHeap,
                                               const std::string& thisPath,
                                               const std::vector<std::string>* callerArgPaths,
                                               uint32_t callerEnvId,
                                               const std::vector<bool>* callerUndefArgs,
                                               bool isConstructCall) {
    if (fn.blocks.empty() || fn.isExternal) {
        return {false, "external or empty function", false, zero64_, {}, {}, zero1_};
    }
    if (inlineDepth > kMaxInlineDepth) {
        return {false, "call recursion depth exceeded", false, zero64_, {}, {}, zero1_};
    }

    SymState initState = moduleState_;
    initState.isConstructCall = isConstructCall;
    initState.threwCond = zero1_;
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

void SymbolicIlBuilder::initModuleState() {
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
    uint32_t savedLoopBound = maxLoopBoundUsed_;
    SymState initSt;
    initSt.threwCond = zero1_;
    initSt.exceededCond = zero1_;
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
    maxLoopBoundUsed_ = savedLoopBound;
}

uint64_t SymbolicIlBuilder::resolveEnvSlotKey(const SymState& st,
                                              il::ValueId envVal,
                                              uint32_t depth,
                                              uint32_t index) const {
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

BVNodeId SymbolicIlBuilder::getVal(const SymState& st, il::ValueId v) const {
    auto it = st.values.find(v);
    return (it != st.values.end()) ? it->second : zero64_;
}

BVNodeId SymbolicIlBuilder::toBool1(BVNodeId node) {
    return (ctx_.width(node) == 1) ? node : ctx_.make_binary(BVOp::Ne, toBV64(node), zero64_);
}

BVNodeId SymbolicIlBuilder::toBV64(BVNodeId node) {
    uint32_t w = ctx_.width(node);
    return (w >= 64) ? node : ((w == 1) ? ctx_.make_zext(node, 64 - w) : ctx_.make_sext(node, 64 - w));
}

BVNodeId SymbolicIlBuilder::toSInt32In64(BVNodeId node) {
    return ctx_.make_sext(ctx_.make_extract(toBV64(node), 31, 0), 32);
}

void SymbolicIlBuilder::applyEdgeArgsInPlace(const il::Function& fn,
                                             const il::BlockTarget& edge,
                                             SymState& state) {
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
        if (state.ctorOf.count(src)) state.ctorOf[dst] = state.ctorOf[src];
        if (state.envIds.count(src)) state.envIds[dst] = state.envIds[src];
        if (state.arrayValues.count(src)) state.arrayValues[dst] = state.arrayValues[src];
    }
    for (const auto& [dst, val] : updates) state.values[dst] = val;
    for (const auto& [dst, isU] : undefUpdates) {
        if (isU) state.undefValues.insert(dst);
        else state.undefValues.erase(dst);
    }
}

void SymbolicIlBuilder::saveModuleInitSnapshot(const SymState& state) {
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

SymEvalOutcome SymbolicIlBuilder::evalBlockPath(const il::Function& fn,
                                                il::BlockId blockId,
                                                SymState state,
                                                uint32_t steps,
                                                uint32_t inlineDepth) {
    const uint32_t maxSteps = inModuleInit_ ? kMaxModuleInitSteps : kMaxBlockSteps;
    const uint32_t maxVisits = inModuleInit_ ? kMaxModuleInitVisits : kMaxTotalBlockVisits;
    std::vector<uint8_t> blockInCycle;
    const uint32_t symLoopBound = inModuleInit_ ? 64u : computeFunctionSymbolicLoopBound(fn, blockInCycle);
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
                    state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64);
                    break;
                case Op::ConstF64: {
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
                        if (state.ctorOf.count(src)) state.ctorOf[inst.result] = state.ctorOf[src];
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
                case Op::GetNewTarget:
                    if (state.isConstructCall) {
                        state.values[inst.result] = one64_;
                    } else {
                        state.values[inst.result] = zero64_;
                        state.undefValues.insert(inst.result);
                    }
                    break;
                case Op::InstanceOf: {
                    if (inst.operands.size() >= 2 && state.ctorOf.count(inst.operands[0]) &&
                        state.funcRefs.count(inst.operands[1])) {
                        state.values[inst.result] =
                            (state.ctorOf[inst.operands[0]] == state.funcRefs[inst.operands[1]]) ? one64_ : zero64_;
                        break;
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
                        return {false, "malformed PropGet", false, zero64_, {}, {}, zero1_};
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
                        return {false, "unsupported PropGet on non-tracked receiver", false, zero64_, {}, {}, zero1_};
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
                        return {false, "unsupported PropSet on non-tracked receiver", false, zero64_, {}, {}, zero1_};
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
                    BVNodeId lhs = toBV64(getVal(state, inst.operands[0]));
                    BVNodeId rhs = toBV64(getVal(state, inst.operands[1]));
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
                    state.values[inst.result] = ctx_.make_binary(bvOp, lhs, rhs);
                    break;
                }
                case Op::Pow: {
                    bool ok = false;
                    BVNodeId r = evalPowSymbolic(ctx_, toBV64(getVal(state, inst.operands[0])),
                                                 toBV64(getVal(state, inst.operands[1])), ok);
                    if (!ok) return {false, "unsupported symbolic exponentiation", false, zero64_, {}, {}, zero1_};
                    state.values[inst.result] = r;
                    break;
                }
                case Op::Div:
                case Op::Mod: {
                    BVNodeId divVal = zero64_;
                    std::string divErr;
                    if (!evalDivOrModSymbolic(ctx_, mod_, fn, blk, instIdx, toBV64(getVal(state, inst.operands[0])),
                                              toBV64(getVal(state, inst.operands[1])), state.heap, divVal, &divErr)) {
                        return {false, divErr, false, zero64_, {}, {}, zero1_};
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
                    if (inst.operands.size() < 2) return {false, "malformed ElemGet", false, zero64_, {}, {}, zero1_};
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
                        return {false, "unsupported dynamic array ElemGet", false, zero64_, {}, {}, zero1_};
                    }
                    if (isUndefElem) state.undefValues.insert(inst.result);
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
                        return {false, "unsupported transcendental MathUnary", false, zero64_, {}, {}, zero1_};
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
                case Op::DynamicCall: {
                    auto sub = evalDynamicCallOp(inst, inlineDepth, state);
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
                    const auto& cNode = ctx_.nodes()[condNode];
                    if (cNode.op == BVOp::Const) {
                        const il::BlockTarget& edge = (cNode.imm != 0) ? inst.target : inst.elseTarget;
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
                        BVNodeId merged = ctx_.make_ite(condNode, trueOut.returnVal, falseOut.returnVal);
                        BVNodeId tThrew = (trueOut.threwNode != 0) ? trueOut.threwNode : zero1_;
                        BVNodeId fThrew = (falseOut.threwNode != 0) ? falseOut.threwNode : zero1_;
                        BVNodeId mergedThrew = (tThrew == fThrew) ? tThrew : ctx_.make_ite(condNode, tThrew, fThrew);
                        BVNodeId mergedExc = (tExc == fExc) ? tExc : ctx_.make_ite(condNode, tExc, fExc);
                        SymbolicHeapState mergedHeap = SymbolicHeapState::mergeBranches(
                            heapTable_, condNode, trueOut.finalHeap, falseOut.finalHeap);
                        std::string retObj = (trueOut.returnObjPath == falseOut.returnObjPath)
                                                 ? trueOut.returnObjPath : std::string{};
                        return {true, {}, true, merged, std::move(mergedHeap), std::move(retObj), mergedThrew, mergedExc};
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
                    BVNodeId pathThrew = (state.threwCond != 0) ? state.threwCond : zero1_;
                    BVNodeId pathExc = (state.exceededCond != 0) ? state.exceededCond : zero1_;
                    return {true, {}, true, ret, std::move(state.heap), std::move(retPath), pathThrew, pathExc};
                }
                default:
                    if (inModuleInit_) {
                        saveModuleInitSnapshot(state);
                        return {true, {}, true, zero64_, std::move(state.heap), {}, zero1_, zero1_};
                    }
                    return {false, std::string("unsupported IL operation: ") + il::opName(inst.op), false, zero64_, {}, {}, zero1_, zero1_};
            }
            if (jumpedInPlace) break;
        }
        if (!jumpedInPlace) {
            return {false, "block terminated without Ret/Jump/Branch", false, zero64_, {}, {}, zero1_, zero1_};
        }
    }
}

bool evalIlFunctionSymbolic(brass::verify::BVContext& ctx,
                            const il::Module& mod,
                            const il::Function& fn,
                            const std::vector<brass::verify::BVNodeId>& argNodes,
                            bool strictJs32Bitwise,
                            brass::verify::BVNodeId* outRetNode,
                            std::string* outReason,
                            SymbolicHeapTable* sharedHeapTable,
                            SymbolicHeapState* outFinalHeap,
                            bool* outHitLoopUnrollBound,
                            brass::verify::BVNodeId* outThrewNode,
                            brass::verify::BVNodeId* outExceededBoundNode,
                            ProofBounds* outBounds) {
    SymbolicIlBuilder builder(ctx, mod, strictJs32Bitwise, sharedHeapTable);
    auto out = builder.evalFunction(fn, argNodes);
    if (outHitLoopUnrollBound) *outHitLoopUnrollBound = builder.hitLoopUnrollBound();
    if (outBounds) *outBounds = builder.boundsUsed();
    if (!out.supported || !out.hasReturn) {
        if (outReason) *outReason = out.unsupportedReason;
        return false;
    }
    if (outRetNode) *outRetNode = out.returnVal;
    if (outFinalHeap) *outFinalHeap = std::move(out.finalHeap);
    if (outThrewNode) *outThrewNode = (out.threwNode != 0) ? out.threwNode : ctx.make_const(0, 1);
    if (outExceededBoundNode) *outExceededBoundNode = (out.exceededBoundNode != 0) ? out.exceededBoundNode : ctx.make_const(0, 1);
    return true;
}

}  // namespace bronze::check
