#include "check/prove_loop_induction.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;

namespace {

bool canReachBlock(const il::Function& fn, il::BlockId start, il::BlockId goal) {
    if (start >= fn.blocks.size() || goal >= fn.blocks.size()) return false;
    if (start == goal) return true;
    std::queue<il::BlockId> q;
    std::unordered_set<il::BlockId> visited{start};
    q.push(start);
    while (!q.empty()) {
        il::BlockId cur = q.front();
        q.pop();
        const auto& blk = fn.blocks[cur];
        if (blk.instructions.empty()) continue;
        const auto& term = blk.instructions.back();
        auto tryPush = [&](il::BlockId next) {
            if (next >= fn.blocks.size()) return false;
            if (next == goal) return true;
            if (visited.insert(next).second) q.push(next);
            return false;
        };
        if (term.op == il::Op::Jump && tryPush(term.target.block)) return true;
        if (term.op == il::Op::Branch && (tryPush(term.target.block) || tryPush(term.elseTarget.block))) return true;
    }
    return false;
}

}  // namespace

bool hasNonPow2ConstDivisor(const il::Function& fn) {
    for (const auto& blk : fn.blocks) {
        std::unordered_map<il::ValueId, int64_t> cv;
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::ConstI32) cv[inst.result] = inst.immI32;
            else if (inst.op == il::Op::ConstF64 && std::isfinite(inst.immF64) && std::trunc(inst.immF64) == inst.immF64)
                cv[inst.result] = static_cast<int64_t>(inst.immF64);
            else if ((inst.op == il::Op::Box || inst.op == il::Op::Unbox ||
                      inst.op == il::Op::ToNumeric || inst.op == il::Op::ToInt32) && !inst.operands.empty()) {
                if (auto it = cv.find(inst.operands[0]); it != cv.end()) cv[inst.result] = it->second;
            } else if ((inst.op == il::Op::Div || inst.op == il::Op::Mod) && inst.operands.size() >= 2) {
                if (auto it = cv.find(inst.operands[1]); it != cv.end() && it->second > 0) {
                    uint64_t d = static_cast<uint64_t>(it->second);
                    if ((d & (d - 1)) != 0) return true;
                }
            }
        }
    }
    return false;
}

LoopIlEvaluator::LoopIlEvaluator(BVContext& ctx,
                                 const il::Module& mod,
                                 bool strictJs32Bitwise,
                                 SymbolicHeapTable* sharedHeapTable)
    : ctx_(ctx),
      mod_(mod),
      strictJs32Bitwise_(strictJs32Bitwise),
      ownedHeapTable_(ctx),
      heapTable_(sharedHeapTable ? *sharedHeapTable : ownedHeapTable_) {
    zero64_ = ctx_.make_const(0, 64);
    one64_ = ctx_.make_const(1, 64);
    allOnes64_ = ctx_.make_const(~uint64_t{0}, 64);
}

bool LoopIlEvaluator::extractSingleLoop(const il::Function& fn,
                                        const std::vector<BVNodeId>& argNodes,
                                        SingleLoopModel& out) {
    if (fn.blocks.empty() || fn.isExternal) return false;
    LoopSymState state;
    uint32_t firstSrc = static_cast<uint32_t>(fn.firstSourceParam());
    for (uint32_t i = 0; i < firstSrc; ++i) state.values[i] = zero64_;
    uint32_t srcCount = static_cast<uint32_t>(fn.callerParamCount());
    for (uint32_t i = 0; i < srcCount; ++i) {
        state.values[firstSrc + i] = (i < argNodes.size()) ? argNodes[i] : zero64_;
    }
    state.heap.initFunctionHeap(heapTable_, fn, state.values);

    il::BlockId cur = 0;
    for (uint32_t steps = 0; steps < 32; ++steps) {
        if (cur >= fn.blocks.size()) return false;
        const auto& blk = fn.blocks[cur];
        if (blk.instructions.empty()) return false;
        const auto& term = blk.instructions.back();

        if (!blk.params.empty() && term.op == il::Op::Branch &&
            canReachBlock(fn, term.target.block, cur) && !canReachBlock(fn, term.elseTarget.block, cur)) {
            out.headerId = cur;
            out.preHeaderState = state;
            out.initParams.resize(blk.params.size(), zero64_);
            for (size_t p = 0; p < blk.params.size(); ++p) out.initParams[p] = getVal(state, blk.params[p].id);
            return analyzeHeader(fn, out);
        }
        if (!execNonTerminators(fn, blk, state)) return false;
        if (term.op == il::Op::Jump) {
            state = applyEdge(fn, state, term.target);
            cur = term.target.block;
        } else if (term.op == il::Op::Branch) {
            BVNodeId condNode = toBool1(toBV64(getVal(state, term.operands[0])));
            const auto& cNode = ctx_.nodes()[condNode];
            if (cNode.op != BVOp::Const) return false;
            const il::BlockTarget& chosen = (cNode.imm != 0) ? term.target : term.elseTarget;
            state = applyEdge(fn, state, chosen);
            cur = chosen.block;
        } else {
            return false;
        }
    }
    return false;
}

bool LoopIlEvaluator::evalHeaderBranch(const il::Function& fn,
                                       const SingleLoopModel& loop,
                                       BVNodeId ivVal,
                                       BVNodeId accVal,
                                       LoopSymState& outSt,
                                       const il::Instruction*& outTerm) {
    const auto& headerBlk = fn.blocks[loop.headerId];
    std::vector<BVNodeId> params = loop.initParams;
    if (loop.ivIdx >= params.size()) return false;
    if (loop.hasScalarAcc) {
        if (loop.accIdx >= params.size()) return false;
        params[loop.accIdx] = accVal;
    }
    params[loop.ivIdx] = ivVal;
    outSt = loop.preHeaderState;
    for (size_t p = 0; p < headerBlk.params.size(); ++p) outSt.values[headerBlk.params[p].id] = params[p];
    if (!execNonTerminators(fn, headerBlk, outSt)) return false;
    outTerm = &headerBlk.instructions.back();
    return outTerm->op == il::Op::Branch;
}

bool LoopIlEvaluator::evalStep(const il::Function& fn,
                               const SingleLoopModel& loop,
                               BVNodeId ivVal,
                               BVNodeId accVal,
                               BVNodeId* outNextAcc,
                               SymbolicHeapState* outStepHeap) {
    LoopSymState st;
    const il::Instruction* term = nullptr;
    if (!evalHeaderBranch(fn, loop, ivVal, accVal, st, term)) return false;
    std::vector<BVNodeId> nextParams;
    SymbolicHeapState endHeap;
    if (!evalToHeader(fn, loop.headerId, term->target.block, applyEdge(fn, st, term->target), 0, nextParams, &endHeap)) {
        return false;
    }
    if (loop.hasScalarAcc) {
        if (loop.accIdx >= nextParams.size()) return false;
        *outNextAcc = nextParams[loop.accIdx];
    } else {
        *outNextAcc = accVal;
    }
    if (outStepHeap) *outStepHeap = std::move(endHeap);
    return true;
}

bool LoopIlEvaluator::evalExit(const il::Function& fn,
                               const SingleLoopModel& loop,
                               BVNodeId ivVal,
                               BVNodeId accVal,
                               BVNodeId* outRetVal) {
    LoopSymState st;
    const il::Instruction* term = nullptr;
    if (!evalHeaderBranch(fn, loop, ivVal, accVal, st, term)) return false;
    return evalToRet(fn, term->elseTarget.block, applyEdge(fn, st, term->elseTarget), 0, outRetVal);
}

BVNodeId LoopIlEvaluator::getVal(const LoopSymState& st, il::ValueId v) const {
    auto it = st.values.find(v);
    return (it != st.values.end()) ? it->second : zero64_;
}

BVNodeId LoopIlEvaluator::toBool1(BVNodeId node) {
    if (ctx_.width(node) == 1) return node;
    return ctx_.make_binary(BVOp::Ne, toBV64(node), zero64_);
}

BVNodeId LoopIlEvaluator::toBV64(BVNodeId node) {
    uint32_t w = ctx_.width(node);
    if (w >= 64) return node;
    if (w == 1) return ctx_.make_zext(node, 64 - w);
    return ctx_.make_sext(node, 64 - w);
}

BVNodeId LoopIlEvaluator::toSInt32In64(BVNodeId node) {
    BVNodeId v32 = ctx_.make_extract(toBV64(node), 31, 0);
    return ctx_.make_sext(v32, 32);
}

LoopSymState LoopIlEvaluator::applyEdge(const il::Function& fn,
                                        const LoopSymState& state,
                                        const il::BlockTarget& edge) {
    LoopSymState nextState = state;
    if (edge.block < fn.blocks.size()) {
        const auto& targetBlk = fn.blocks[edge.block];
        for (size_t k = 0; k < targetBlk.params.size() && k < edge.args.size(); ++k) {
            nextState.values[targetBlk.params[k].id] = toBV64(getVal(state, edge.args[k]));
            nextState.heap.propagateAlias(edge.args[k], targetBlk.params[k].id);
        }
    }
    return nextState;
}

bool LoopIlEvaluator::analyzeHeader(const il::Function& fn, SingleLoopModel& out) {
    const auto& headerBlk = fn.blocks[out.headerId];
    const size_t K = headerBlk.params.size();
    if (K < 1) return false;

    BVContext probeCtx = ctx_;
    LoopIlEvaluator probeEval(probeCtx, mod_, strictJs32Bitwise_);

    std::vector<BVNodeId> probeParams(K);
    for (size_t p = 0; p < K; ++p) {
        BVNodeId v32 = probeCtx.make_var("__loop_probe_" + std::to_string(p), 32);
        probeParams[p] = probeCtx.make_sext(v32, 32);
    }

    LoopSymState st = out.preHeaderState;
    for (size_t p = 0; p < K; ++p) {
        st.values[headerBlk.params[p].id] = probeParams[p];
    }
    if (!probeEval.execNonTerminators(fn, headerBlk, st)) return false;

    const auto& term = headerBlk.instructions.back();
    if (term.op != il::Op::Branch || term.operands.empty()) return false;
    BVNodeId rawCond = probeEval.toBV64(probeEval.getVal(st, term.operands[0]));

    LoopSymState bodyState = probeEval.applyEdge(fn, st, term.target);
    std::vector<BVNodeId> nextProbeParams;
    if (!probeEval.evalToHeader(fn, out.headerId, term.target.block, bodyState, 0, nextProbeParams)) {
        return false;
    }
    if (nextProbeParams.size() != K) return false;

    BVNodeId condInner = rawCond;
    if (probeCtx.nodes()[condInner].op == BVOp::ZExt) {
        condInner = probeCtx.nodes()[condInner].a;
    }
    bool isLe = false;
    if (probeCtx.nodes()[condInner].op == BVOp::Not) {
        condInner = probeCtx.nodes()[condInner].a;
        isLe = true;
    }
    const auto& cNode = probeCtx.nodes()[condInner];
    if (cNode.op != BVOp::Slt && cNode.op != BVOp::Sle) return false;
    if (cNode.op == BVOp::Sle) isLe = true;

    BVNodeId lhs64 = probeEval.toBV64(isLe && cNode.op == BVOp::Slt ? cNode.b : cNode.a);
    BVNodeId rhsRaw = (isLe && cNode.op == BVOp::Slt) ? cNode.a : cNode.b;

    bool foundIv = false;
    for (size_t p = 0; p < K; ++p) {
        if (lhs64 == probeParams[p] &&
            out.initParams[p] == zero64_ &&
            rhsRaw < ctx_.nodes().size() &&
            nextProbeParams[p] == probeCtx.make_binary(BVOp::Add, probeParams[p], one64_)) {
            BVNodeId rhs64 = toBV64(rhsRaw);
            out.ivIdx = p;
            out.boundNode = isLe ? ctx_.make_binary(BVOp::Add, rhs64, one64_) : rhs64;
            foundIv = true;
            break;
        }
    }
    if (!foundIv) return false;

    size_t activeCount = 0;
    size_t candidateAcc = (out.ivIdx == 0) ? (K > 1 ? 1 : 0) : 0;
    for (size_t p = 0; p < K; ++p) {
        if (p == out.ivIdx) continue;
        if (nextProbeParams[p] != probeParams[p]) {
            candidateAcc = p;
            ++activeCount;
        }
    }
    if (activeCount > 1) return false;
    out.accIdx = candidateAcc;
    out.hasScalarAcc = (activeCount == 1 || (K >= 2 && candidateAcc != out.ivIdx));

    LoopSymState exitState = probeEval.applyEdge(fn, st, term.elseTarget);
    BVNodeId probeExitRet = zero64_;
    if (!probeEval.evalToRet(fn, term.elseTarget.block, exitState, 0, &probeExitRet)) {
        return false;
    }
    out.exitIsIdentity = out.hasScalarAcc && (probeExitRet == probeParams[out.accIdx]);
    return true;
}

bool LoopIlEvaluator::evalToHeader(const il::Function& fn,
                                   il::BlockId headerId,
                                   il::BlockId cur,
                                   LoopSymState state,
                                   uint32_t steps,
                                   std::vector<BVNodeId>& outLatchParams,
                                   SymbolicHeapState* outHeap) {
    if (steps > 24 || cur >= fn.blocks.size()) return false;
    const auto& blk = fn.blocks[cur];
    if (blk.instructions.empty() || !execNonTerminators(fn, blk, state)) return false;

    const auto& term = blk.instructions.back();
    const size_t K = fn.blocks[headerId].params.size();
    auto extractLatchArgs = [&](const il::BlockTarget& edge, std::vector<BVNodeId>& dst) {
        if (edge.args.size() < K) return false;
        dst.resize(K);
        for (size_t k = 0; k < K; ++k) dst[k] = toBV64(getVal(state, edge.args[k]));
        if (outHeap) *outHeap = state.heap;
        return true;
    };

    if (term.op == il::Op::Jump) {
        if (term.target.block == headerId) return extractLatchArgs(term.target, outLatchParams);
        return evalToHeader(fn, headerId, term.target.block, applyEdge(fn, state, term.target), steps + 1, outLatchParams, outHeap);
    }
    if (term.op == il::Op::Branch) {
        BVNodeId condNode = toBool1(toBV64(getVal(state, term.operands[0])));
        const auto& cNode = ctx_.nodes()[condNode];
        if (cNode.op == BVOp::Const) {
            const il::BlockTarget& chosen = (cNode.imm != 0) ? term.target : term.elseTarget;
            if (chosen.block == headerId) return extractLatchArgs(chosen, outLatchParams);
            return evalToHeader(fn, headerId, chosen.block, applyEdge(fn, state, chosen), steps + 1, outLatchParams, outHeap);
        }
        std::vector<BVNodeId> tParams, fParams;
        SymbolicHeapState tHeap, fHeap;
        bool okT = (term.target.block == headerId)
                       ? (tHeap = state.heap, extractLatchArgs(term.target, tParams))
                       : evalToHeader(fn, headerId, term.target.block, applyEdge(fn, state, term.target), steps + 1, tParams, &tHeap);
        if (!okT) return false;
        bool okF = (term.elseTarget.block == headerId)
                       ? (fHeap = state.heap, extractLatchArgs(term.elseTarget, fParams))
                       : evalToHeader(fn, headerId, term.elseTarget.block, applyEdge(fn, state, term.elseTarget), steps + 1, fParams, &fHeap);
        if (!okF || tParams.size() != K || fParams.size() != K) return false;
        outLatchParams.resize(K);
        for (size_t k = 0; k < K; ++k) outLatchParams[k] = ctx_.make_ite(condNode, tParams[k], fParams[k]);
        if (outHeap) *outHeap = SymbolicHeapState::mergeBranches(heapTable_, condNode, tHeap, fHeap);
        return true;
    }
    return false;
}

bool LoopIlEvaluator::evalToRet(const il::Function& fn,
                                il::BlockId cur,
                                LoopSymState state,
                                uint32_t steps,
                                BVNodeId* outRet) {
    if (steps > 24 || cur >= fn.blocks.size()) return false;
    const auto& blk = fn.blocks[cur];
    if (blk.instructions.empty() || !execNonTerminators(fn, blk, state)) return false;

    const auto& term = blk.instructions.back();
    if (term.op == il::Op::Ret) {
        *outRet = term.operands.empty() ? zero64_ : toBV64(getVal(state, term.operands[0]));
        return true;
    }
    if (term.op == il::Op::Jump) {
        return evalToRet(fn, term.target.block, applyEdge(fn, state, term.target), steps + 1, outRet);
    }
    if (term.op == il::Op::Branch) {
        BVNodeId condNode = toBool1(toBV64(getVal(state, term.operands[0])));
        const auto& cNode = ctx_.nodes()[condNode];
        if (cNode.op == BVOp::Const) {
            const il::BlockTarget& chosen = (cNode.imm != 0) ? term.target : term.elseTarget;
            return evalToRet(fn, chosen.block, applyEdge(fn, state, chosen), steps + 1, outRet);
        }
        BVNodeId tRet = zero64_, fRet = zero64_;
        if (!evalToRet(fn, term.target.block, applyEdge(fn, state, term.target), steps + 1, &tRet) ||
            !evalToRet(fn, term.elseTarget.block, applyEdge(fn, state, term.elseTarget), steps + 1, &fRet)) {
            return false;
        }
        *outRet = ctx_.make_ite(condNode, tRet, fRet);
        return true;
    }
    return false;
}

bool LoopIlEvaluator::execNonTerminators(const il::Function& fn, const il::Block& blk, LoopSymState& state) {
    if (blk.instructions.empty()) return false;
    for (size_t idx = 0; idx + 1 < blk.instructions.size(); ++idx) {
        const auto& inst = blk.instructions[idx];
        using Op = il::Op;
        auto op64 = [&](size_t k) {
            return (k < inst.operands.size()) ? toBV64(getVal(state, inst.operands[k])) : zero64_;
        };
        auto op32 = [&](size_t k) { return ctx_.make_extract(op64(k), 31, 0); };
        switch (inst.op) {
            case Op::ConstI32:
                state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64);
                break;
            case Op::ConstF64:
                if (!std::isfinite(inst.immF64) || std::trunc(inst.immF64) != inst.immF64) return false;
                state.values[inst.result] = ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immF64)), 64);
                break;
            case Op::ConstBool:
                state.values[inst.result] = (inst.immI32 != 0) ? one64_ : zero64_;
                break;
            case Op::ConstNull:
            case Op::ConstUndefined:
            case Op::IsNullish:
            case Op::EnvCreate:
                state.values[inst.result] = zero64_;
                break;
            case Op::ToInt32:
                state.values[inst.result] = toSInt32In64(op64(0));
                break;
            case Op::Box:
            case Op::Unbox:
            case Op::ToNumeric:
            case Op::NumTruthy:
                state.values[inst.result] = op64(0);
                if (!inst.operands.empty()) state.heap.propagateAlias(inst.operands[0], inst.result);
                break;
            case Op::PinGuard:
            case Op::CensusRecord:
            case Op::FeedbackRecord:
            case Op::KeepAlive:
            case Op::EnvInitTdz:
                break;
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
            case Op::CreateObject:
                state.values[inst.result] = state.heap.createLocalObject(heapTable_, inst.result);
                break;
            case Op::PropGet: {
                if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) return false;
                BVNodeId pv = zero64_;
                if (!state.heap.evalPropGet(heapTable_, inst.operands[0], mod_.keyConstants[inst.keyIndex], inst.result, pv)) {
                    return false;
                }
                state.values[inst.result] = pv;
                break;
            }
            case Op::PropSet:
            case Op::DefineOwnAttr: {
                if (inst.operands.size() < 2 || inst.keyIndex >= mod_.keyConstants.size()) return false;
                BVNodeId vn = op64(1);
                if (!state.heap.evalPropSet(heapTable_, inst.operands[0], mod_.keyConstants[inst.keyIndex], vn, inst.operands[1])) {
                    return false;
                }
                if (inst.result != il::kNoValue) state.values[inst.result] = vn;
                break;
            }
            case Op::ElemGet:
            case Op::ElemGetTyped: {
                if (inst.operands.size() < 2) return false;
                BVNodeId ev = zero64_;
                if (!state.heap.evalElemGet(heapTable_, inst.operands[0], op64(1), inst.result, ev)) return false;
                state.values[inst.result] = ev;
                break;
            }
            case Op::ElemSet:
            case Op::ElemSetTyped: {
                if (inst.operands.size() < 3) return false;
                BVNodeId vn = op64(2);
                if (!state.heap.evalElemSet(heapTable_, inst.operands[0], op64(1), vn, inst.operands[2])) return false;
                if (inst.result != il::kNoValue) state.values[inst.result] = vn;
                break;
            }
            case Op::NumericStep:
                state.values[inst.result] = ctx_.make_binary(
                    BVOp::Add, op64(0), ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(inst.immI32)), 64));
                break;
            case Op::MathImul:
                state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(BVOp::Mul, op32(0), op32(1)), 32);
                break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul: {
                BVOp bvOp = (inst.op == Op::Add) ? BVOp::Add : (inst.op == Op::Sub ? BVOp::Sub : BVOp::Mul);
                state.values[inst.result] = ctx_.make_binary(bvOp, op64(0), op64(1));
                break;
            }
            case Op::Div:
            case Op::Mod: {
                BVNodeId divVal = zero64_;
                if (!evalDivOrModSymbolic(ctx_, mod_, fn, blk, idx, op64(0), op64(1), state.heap, divVal)) {
                    return false;
                }
                state.values[inst.result] = divVal;
                break;
            }
            case Op::Neg:
                state.values[inst.result] = ctx_.make_unary(BVOp::Neg, op64(0));
                break;
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor: {
                BVOp op = (inst.op == Op::BitAnd) ? BVOp::And : (inst.op == Op::BitOr ? BVOp::Or : BVOp::Xor);
                state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(op, op32(0), op32(1)), 32);
                break;
            }
            case Op::BitNot:
                state.values[inst.result] = ctx_.make_sext(ctx_.make_unary(BVOp::Not, op32(0)), 32);
                break;
            case Op::Shl: {
                BVNodeId sh32 = ctx_.make_binary(BVOp::And, op32(1), ctx_.make_const(31, 32));
                state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(BVOp::Shl, op32(0), sh32), 32);
                break;
            }
            case Op::Shr:
            case Op::UShr: {
                BVNodeId sh32 = ctx_.make_binary(BVOp::And, op32(1), ctx_.make_const(31, 32));
                if (inst.op == Op::Shr) {
                    state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(BVOp::AShr, op32(0), sh32), 32);
                } else {
                    state.values[inst.result] = ctx_.make_zext(ctx_.make_binary(BVOp::LShr, op32(0), sh32), 32);
                }
                break;
            }
            case Op::CmpEq:
            case Op::StrictEq:
            case Op::LooseEq:
            case Op::CmpNe:
            case Op::CmpLt:
            case Op::RelLt:
            case Op::CmpLe:
            case Op::RelLe:
            case Op::CmpGt:
            case Op::RelGt:
            case Op::CmpGe:
            case Op::RelGe: {
                BVNodeId a = op64(0), b = op64(1);
                if (inst.op == Op::CmpGt || inst.op == Op::RelGt || inst.op == Op::CmpGe || inst.op == Op::RelGe) {
                    std::swap(a, b);
                }
                BVOp cop = (inst.op == Op::CmpEq || inst.op == Op::StrictEq || inst.op == Op::LooseEq)
                               ? BVOp::Eq
                               : (inst.op == Op::CmpNe
                                      ? BVOp::Ne
                                      : ((inst.op == Op::CmpLt || inst.op == Op::RelLt ||
                                          inst.op == Op::CmpGt || inst.op == Op::RelGt)
                                             ? BVOp::Slt
                                             : BVOp::Sle));
                state.values[inst.result] = toBV64(ctx_.make_binary(cop, a, b));
                break;
            }
            case Op::MathUnary: {
                BVNodeId a = op64(0);
                auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                if (kind == il::MathUnaryFn::Abs) {
                    state.values[inst.result] = ctx_.make_ite(
                        ctx_.make_binary(BVOp::Slt, a, zero64_), ctx_.make_unary(BVOp::Neg, a), a);
                } else if (kind == il::MathUnaryFn::Floor || kind == il::MathUnaryFn::Ceil) {
                    state.values[inst.result] = evalFloorOrCeilSymbolic(
                        ctx_, state.heap, inst.operands[0], a, kind == il::MathUnaryFn::Ceil);
                } else if (kind == il::MathUnaryFn::Trunc) {
                    state.values[inst.result] = a;
                } else {
                    return false;
                }
                break;
            }
            case Op::GlobalGet:
                if (inst.keyIndex < mod_.keyConstants.size()) {
                    state.globalRefs[inst.result] = mod_.keyConstants[inst.keyIndex];
                }
                state.values[inst.result] = zero64_;
                break;
            case Op::MethodCall: {
                if (inst.operands.empty() || inst.keyIndex >= mod_.keyConstants.size()) return false;
                auto recvIt = state.globalRefs.find(inst.operands[0]);
                const std::string& m = mod_.keyConstants[inst.keyIndex];
                if (recvIt != state.globalRefs.end() && recvIt->second == "Math") {
                    if (m == "imul" && inst.operands.size() >= 3) {
                        state.values[inst.result] = ctx_.make_sext(ctx_.make_binary(BVOp::Mul, op32(1), op32(2)), 32);
                        break;
                    }
                    if ((m == "floor" || m == "ceil") && inst.operands.size() >= 2) {
                        state.values[inst.result] = evalFloorOrCeilSymbolic(
                            ctx_, state.heap, inst.operands[1], op64(1), m == "ceil");
                        break;
                    }
                    if ((m == "trunc" || m == "round" || m == "fround") && inst.operands.size() >= 2) {
                        state.values[inst.result] = op64(1);
                        break;
                    }
                }
                if (recvIt != state.globalRefs.end() && recvIt->second == "Number" && inst.operands.size() >= 2) {
                    if (m == "isFinite" || m == "isSafeInteger" || m == "isInteger") {
                        state.values[inst.result] = one64_;
                        break;
                    }
                    if (m == "isNaN") {
                        state.values[inst.result] = zero64_;
                        break;
                    }
                }
                return false;
            }
            case Op::EnvSet:
                if (inst.operands.size() >= 2) {
                    uint64_t ek = (static_cast<uint64_t>(inst.envDepth) << 32) | inst.envIndex;
                    state.envSlots[ek] = op64(1);
                    state.heap.propagateEnvSet(ek, inst.operands[1]);
                }
                break;
            case Op::EnvGet:
            case Op::EnvGetTdz: {
                uint64_t ek = (static_cast<uint64_t>(inst.envDepth) << 32) | inst.envIndex;
                auto it = state.envSlots.find(ek);
                state.values[inst.result] = (it != state.envSlots.end()) ? it->second : zero64_;
                state.heap.propagateEnvGet(ek, inst.result);
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

}  // namespace bronze::check
