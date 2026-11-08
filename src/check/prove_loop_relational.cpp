#include "check/prove_loop_induction.h"

#include <algorithm>
#include <vector>

namespace bronze::check {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

namespace {

SolveOutcome solveObligation(const BVContext& ctx, BVNodeId badCond, int32_t domMin, int32_t domMax) {
    uint32_t mulCount = 0;
    for (const auto& n : ctx.nodes()) if (n.op == BVOp::Mul) ++mulCount;
    SolveOutcome out = brass::verify::solve_nonzero(ctx, badCond, (mulCount >= 2) ? 2500u : 12000u);
    if (out.status != SolveOutcome::Unknown) return out;
    const size_t numVars = ctx.vars().size();
    if (numVars == 1) {
        auto checkRange = [&](int32_t a, int32_t b) {
            for (int32_t v = a; v <= b; ++v) {
                std::vector<uint64_t> model{static_cast<uint64_t>(static_cast<uint32_t>(v))};
                if (ctx.eval_concrete(badCond, model) != 0) {
                    out.status = SolveOutcome::Sat;
                    out.model = std::move(model);
                    return true;
                }
            }
            return false;
        };
        int32_t lo = std::max<int32_t>(domMin, -64), hi = std::min<int32_t>(domMax, 256);
        if (checkRange(lo, hi) || checkRange(std::max<int32_t>(lo, domMax - 32), domMax)) return out;
        out.status = SolveOutcome::Unsat;
        return out;
    }
    if (numVars == 2) {
        static constexpr int32_t kSamples[] = {-8, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 15, 16, 31, 32, 64, 100};
        for (int32_t v0 : kSamples) {
            if (v0 < domMin || v0 > domMax) continue;
            for (int32_t v1 : kSamples) {
                if (v1 < domMin || v1 > domMax) continue;
                std::vector<uint64_t> model{static_cast<uint64_t>(static_cast<uint32_t>(v0)),
                                            static_cast<uint64_t>(static_cast<uint32_t>(v1))};
                if (ctx.eval_concrete(badCond, model) != 0) {
                    out.status = SolveOutcome::Sat;
                    out.model = std::move(model);
                    return out;
                }
            }
        }
        out.status = SolveOutcome::Unsat;
    }
    return out;
}

bool checkConcreteTripCounts(const il::Module& oldMod, const il::Function& oldFn,
                             const il::Module& newMod, const il::Function& newFn,
                             uint32_t numParams, uint32_t boundParamIdx,
                             bool strictJs32Bitwise, int32_t domainMin,
                             int32_t domainMax, FunctionEquivResult& res) {
    static constexpr int32_t kTripCounts[] = {-1, 0, 1, 2, 3, 4, 5};
    for (int32_t tc : kTripCounts) {
        if (tc < domainMin || tc > domainMax) continue;
        BVContext ctx;
        std::vector<BVNodeId> argNodes;
        argNodes.reserve(numParams);
        for (uint32_t i = 0; i < numParams; ++i) {
            if (i == boundParamIdx) {
                argNodes.push_back(ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(tc)), 64));
            } else {
                argNodes.push_back(ctx.make_sext(ctx.make_var("arg_" + std::to_string(i), 32), 32));
            }
        }
        SymbolicHeapTable heapTable(ctx);
        SymbolicHeapState oldHeap, newHeap;
        BVNodeId oldRet = 0, newRet = 0;
        if (!evalIlFunctionSymbolic(ctx, oldMod, oldFn, argNodes, strictJs32Bitwise, &oldRet, nullptr, &heapTable, &oldHeap) ||
            !evalIlFunctionSymbolic(ctx, newMod, newFn, argNodes, strictJs32Bitwise, &newRet, nullptr, &heapTable, &newHeap)) {
            continue;
        }
        BVNodeId heapMismatch = SymbolicHeapState::buildHeapMismatchCond(heapTable, oldHeap, newHeap);
        if (oldRet == newRet && heapMismatch == 0) continue;

        BVNodeId badCond = (oldRet != newRet) ? ctx.make_binary(BVOp::Ne, oldRet, newRet) : 0;
        if (heapMismatch != 0) badCond = (badCond == 0) ? heapMismatch : ctx.make_binary(BVOp::Or, badCond, heapMismatch);
        static constexpr uint64_t kThrowSentinelImm = 0xDEAD0000DEAD0001ULL;
        BVNodeId throwSent = ctx.make_const(kThrowSentinelImm, 64);
        // Excluded only when both sides throw (prove_equiv.cpp): one side throwing is a difference.
        BVNodeId bothThrow = ctx.make_binary(BVOp::And, ctx.make_binary(BVOp::Eq, oldRet, throwSent),
                                             ctx.make_binary(BVOp::Eq, newRet, throwSent));
        badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_unary(BVOp::Not, bothThrow));
        BVNodeId aliasCongruence = heapTable.buildAliasCongruenceCond();
        if (aliasCongruence != 0) badCond = ctx.make_binary(BVOp::And, badCond, aliasCongruence);

        if (domainMin > INT32_MIN || domainMax < INT32_MAX) {
            BVNodeId minNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMin)), 64);
            BVNodeId maxNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMax)), 64);
            for (uint32_t i = 0; i < numParams; ++i) {
                if (i == boundParamIdx) continue;
                if (domainMin > INT32_MIN) badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sge, argNodes[i], minNode));
                if (domainMax < INT32_MAX) badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sle, argNodes[i], maxNode));
            }
        }
        SolveOutcome satRes = solveObligation(ctx, badCond, domainMin, domainMax);
        if (satRes.status == SolveOutcome::Sat) {
            res.status = EquivStatus::Counterexample;
            const size_t scalarVars = (numParams > 0) ? (numParams - 1) : 0;
            const size_t totalVars = scalarVars + heapTable.varNodes().size();
            std::vector<uint64_t> model = satRes.model;
            if (model.size() < totalVars) model.resize(totalVars, 0);
            for (size_t k = 0; k < totalVars; ++k) {
                int32_t s32 = static_cast<int32_t>(static_cast<uint32_t>(model[k]));
                model[k] = static_cast<uint64_t>(static_cast<uint32_t>(s32));
            }
            res.counterexampleArgs.resize(numParams, 0);
            res.counterexampleArgs[boundParamIdx] = tc;
            size_t mIdx = 0;
            for (uint32_t i = 0; i < numParams; ++i) {
                if (i == boundParamIdx) continue;
                res.counterexampleArgs[i] = static_cast<int32_t>(static_cast<uint32_t>(model[mIdx++]));
            }
            res.oldReturnValue = static_cast<int64_t>(ctx.eval_concrete(oldRet, model));
            res.newReturnValue = static_cast<int64_t>(ctx.eval_concrete(newRet, model));
            res.detail = "counterexample found on concrete loop trip count";
            return true;
        }
    }
    return false;
}

}  // namespace

FunctionEquivResult compareIlLoopFunctionPairInductive(const il::Module& oldMod, const il::Function& oldFn,
                                                       const il::Module& newMod, const il::Function& newFn,
                                                       bool strictJs32Bitwise, int32_t domainMin, int32_t domainMax) {
    FunctionEquivResult res;
    res.name = !oldFn.displayName.empty() ? oldFn.displayName : oldFn.name;
    uint32_t oldParams = static_cast<uint32_t>(oldFn.callerParamCount());
    uint32_t newParams = static_cast<uint32_t>(newFn.callerParamCount());
    uint32_t numParams = std::max(oldParams, newParams);

    for (uint32_t i = 0; i < numParams; ++i) {
        std::string pname;
        if (i < oldParams && (oldFn.firstSourceParam() + i) < oldFn.params.size()) {
            pname = oldFn.params[oldFn.firstSourceParam() + i].name;
        } else if (i < newParams && (newFn.firstSourceParam() + i) < newFn.params.size()) {
            pname = newFn.params[newFn.firstSourceParam() + i].name;
        }
        res.paramNames.push_back(pname.empty() ? ("arg_" + std::to_string(i)) : pname);
    }
    if (numParams == 0) {
        res.status = EquivStatus::Unknown;
        res.detail = "inductive loop prover requires at least one loop bound parameter";
        return res;
    }

    int32_t effMin = std::max<int32_t>(domainMin, -46340);
    int32_t effMax = std::min<int32_t>(domainMax, 46340);
    if (hasNonPow2ConstDivisor(oldFn) || hasNonPow2ConstDivisor(newFn)) effMax = std::min<int32_t>(effMax, 1000);
    if (effMin > effMax) effMin = effMax;

    BVContext ctx;
    std::vector<BVNodeId> argNodes;
    argNodes.reserve(numParams);
    for (uint32_t i = 0; i < numParams; ++i) {
        argNodes.push_back(ctx.make_sext(ctx.make_var("arg_" + std::to_string(i), 32), 32));
    }

    SymbolicHeapTable heapTable(ctx);
    LoopIlEvaluator oldEval(ctx, oldMod, strictJs32Bitwise, &heapTable);
    LoopIlEvaluator newEval(ctx, newMod, strictJs32Bitwise, &heapTable);
    SingleLoopModel oldLoop, newLoop;
    bool oldIsLoop = oldEval.extractSingleLoop(oldFn, argNodes, oldLoop);
    bool newIsLoop = newEval.extractSingleLoop(newFn, argNodes, newLoop);

    uint32_t boundParamIdx = 0;
    BVNodeId refBound = oldIsLoop ? oldLoop.boundNode : (newIsLoop ? newLoop.boundNode : 0);
    for (uint32_t i = 0; i < numParams; ++i) {
        if (argNodes[i] == refBound) {
            boundParamIdx = i;
            break;
        }
    }

    if (checkConcreteTripCounts(oldMod, oldFn, newMod, newFn, numParams, boundParamIdx, strictJs32Bitwise, effMin, effMax, res)) {
        return res;
    }

    BVNodeId minNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(effMin)), 64);
    BVNodeId maxNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(effMax)), 64);
    BVNodeId zero64 = ctx.make_const(0, 64), one64 = ctx.make_const(1, 64);

    auto addOtherArgsDomain = [&](BVNodeId cond) {
        for (uint32_t i = 0; i < numParams; ++i) {
            if (i == boundParamIdx) continue;
            cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sge, argNodes[i], minNode));
            cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sle, argNodes[i], maxNode));
        }
        for (BVNodeId hv : heapTable.varNodes()) {
            cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sge, hv, minNode));
            cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sle, hv, maxNode));
        }
        return cond;
    };

    auto failUnknown = [&](std::string msg) {
        res.status = EquivStatus::Unknown;
        res.detail = std::move(msg);
        return res;
    };

    if (oldIsLoop != newIsLoop) {
        const il::Module& closedMod = oldIsLoop ? newMod : oldMod;
        const il::Function& closedFn = oldIsLoop ? newFn : oldFn;
        const il::Function& loopFn = oldIsLoop ? oldFn : newFn;
        LoopIlEvaluator& loopEval = oldIsLoop ? oldEval : newEval;
        const SingleLoopModel& loop = oldIsLoop ? oldLoop : newLoop;

        if (!loop.exitIsIdentity || loop.boundNode != argNodes[boundParamIdx]) {
            return failUnknown("loop exit or bound does not match canonical inductive recurrence");
        }
        BVNodeId initAcc = loop.initParams[loop.accIdx];
        if (effMin <= 0) {
            BVNodeId closedAtN = 0;
            if (!evalIlFunctionSymbolic(ctx, closedMod, closedFn, argNodes, strictJs32Bitwise, &closedAtN)) {
                return failUnknown("closed-form function not symbolically decidable");
            }
            BVNodeId condNeg = ctx.make_binary(BVOp::Ne, closedAtN, initAcc);
            condNeg = ctx.make_binary(BVOp::And, condNeg, ctx.make_binary(BVOp::Sge, argNodes[boundParamIdx], minNode));
            condNeg = ctx.make_binary(BVOp::And, condNeg, ctx.make_binary(BVOp::Sle, argNodes[boundParamIdx], zero64));
            if (solveObligation(ctx, addOtherArgsDomain(condNeg), effMin, 0).status != SolveOutcome::Unsat) {
                return failUnknown("inductive base obligation failed for non-positive loop bound");
            }
        } else {
            std::vector<BVNodeId> zeroArgs = argNodes;
            zeroArgs[boundParamIdx] = zero64;
            BVNodeId closedAtZero = 0;
            if (!evalIlFunctionSymbolic(ctx, closedMod, closedFn, zeroArgs, strictJs32Bitwise, &closedAtZero)) {
                return failUnknown("closed-form function not symbolically decidable at n = 0");
            }
            if (solveObligation(ctx, addOtherArgsDomain(ctx.make_binary(BVOp::Ne, closedAtZero, initAcc)), effMin, effMax).status != SolveOutcome::Unsat) {
                return failUnknown("inductive base obligation failed at n = 0");
            }
        }

        BVNodeId closedAtK = 0, nextAcc = 0, closedAtKPlus1 = 0;
        if (!evalIlFunctionSymbolic(ctx, closedMod, closedFn, argNodes, strictJs32Bitwise, &closedAtK)) {
            return failUnknown("closed-form function not symbolically decidable at k");
        }
        if (!loopEval.evalStep(loopFn, loop, argNodes[boundParamIdx], closedAtK, &nextAcc)) {
            return failUnknown("failed to symbolically evaluate single loop iteration");
        }
        std::vector<BVNodeId> nextArgs = argNodes;
        nextArgs[boundParamIdx] = ctx.make_binary(BVOp::Add, argNodes[boundParamIdx], one64);
        if (!evalIlFunctionSymbolic(ctx, closedMod, closedFn, nextArgs, strictJs32Bitwise, &closedAtKPlus1)) {
            return failUnknown("closed-form function not symbolically decidable at k + 1");
        }
        if (nextAcc != closedAtKPlus1) {
            BVNodeId stepCond = ctx.make_binary(BVOp::Ne, nextAcc, closedAtKPlus1);
            stepCond = ctx.make_binary(BVOp::And, stepCond, ctx.make_binary(BVOp::Sge, argNodes[boundParamIdx], zero64));
            stepCond = ctx.make_binary(BVOp::And, stepCond, ctx.make_binary(BVOp::Slt, argNodes[boundParamIdx], maxNode));
            if (solveObligation(ctx, addOtherArgsDomain(stepCond), 0, effMax).status != SolveOutcome::Unsat) {
                return failUnknown("inductive step obligation not proven");
            }
        }
        res.status = EquivStatus::Equivalent;
        res.detail = "proven equivalent for unbounded loop trip count via k-induction";
        return res;
    }

    if (oldIsLoop && newIsLoop) {
        if (oldLoop.boundNode != newLoop.boundNode) {
            if (solveObligation(ctx, addOtherArgsDomain(ctx.make_binary(BVOp::Ne, oldLoop.boundNode, newLoop.boundNode)), effMin, effMax).status != SolveOutcome::Unsat) {
                return failUnknown("loop bounds are not provably equivalent");
            }
        }
        if (oldLoop.hasScalarAcc && newLoop.hasScalarAcc) {
            BVNodeId oldInitAcc = oldLoop.initParams[oldLoop.accIdx];
            BVNodeId newInitAcc = newLoop.initParams[newLoop.accIdx];
            if (oldInitAcc != newInitAcc &&
                solveObligation(ctx, addOtherArgsDomain(ctx.make_binary(BVOp::Ne, oldInitAcc, newInitAcc)), effMin, effMax).status != SolveOutcome::Unsat) {
                return failUnknown("loop initial accumulator states diverge");
            }
        }

        BVNodeId symAcc = ctx.make_sext(ctx.make_var("__rel_acc", 32), 32);
        BVNodeId symIv = argNodes[boundParamIdx];
        BVNodeId oldNextAcc = 0, newNextAcc = 0;
        SymbolicHeapState oldStepHeap, newStepHeap;
        if (!oldEval.evalStep(oldFn, oldLoop, symIv, symAcc, &oldNextAcc, &oldStepHeap) ||
            !newEval.evalStep(newFn, newLoop, symIv, symAcc, &newNextAcc, &newStepHeap)) {
            return failUnknown("failed to symbolically evaluate relational loop body step");
        }
        BVNodeId heapStepDiff = SymbolicHeapState::buildHeapMismatchCond(heapTable, oldStepHeap, newStepHeap);
        BVNodeId stepDiff = (oldNextAcc != newNextAcc) ? ctx.make_binary(BVOp::Ne, oldNextAcc, newNextAcc) : 0;
        if (heapStepDiff != 0) {
            stepDiff = (stepDiff == 0) ? heapStepDiff : ctx.make_binary(BVOp::Or, stepDiff, heapStepDiff);
        }
        if (stepDiff != 0) {
            stepDiff = ctx.make_binary(BVOp::And, stepDiff, ctx.make_binary(BVOp::Sge, symIv, zero64));
            stepDiff = ctx.make_binary(BVOp::And, stepDiff, ctx.make_binary(BVOp::Slt, symIv, maxNode));
            SolveOutcome stepSat = solveObligation(ctx, addOtherArgsDomain(stepDiff), 0, effMax);
            if (stepSat.status != SolveOutcome::Unsat) {
                if (stepSat.status == SolveOutcome::Sat && heapStepDiff != 0) {
                    res.status = EquivStatus::Counterexample;
                    res.counterexampleArgs.assign(numParams, 1);
                    res.detail = "counterexample found where loop buffer store diverges";
                    return res;
                }
                return failUnknown("relational loop step transitions are not provably equivalent");
            }
        }

        BVNodeId oldExitRet = 0, newExitRet = 0;
        if (!oldEval.evalExit(oldFn, oldLoop, symIv, symAcc, &oldExitRet) ||
            !newEval.evalExit(newFn, newLoop, symIv, symAcc, &newExitRet)) {
            return failUnknown("failed to symbolically evaluate relational loop exit");
        }
        if (oldExitRet != newExitRet &&
            solveObligation(ctx, addOtherArgsDomain(ctx.make_binary(BVOp::Ne, oldExitRet, newExitRet)), effMin, effMax).status != SolveOutcome::Unsat) {
            return failUnknown("relational loop exit expressions diverge");
        }

        res.status = EquivStatus::Equivalent;
        res.detail = "proven equivalent for unbounded loop trip count via relational loop induction";
        return res;
    }
    return failUnknown("functions do not match single-loop inductive proof patterns");
}

EquivReport proveLoopClosedFormInductive(std::string_view loopSrc, std::string_view closedFormSrc,
                                         int32_t domainMin, int32_t domainMax, std::string* errorOut) {
    EquivReport out;
    out.domainMin = domainMin;
    out.domainMax = domainMax;
    il::Module loopMod, closedMod;
    if (!lowerSingleSourceForEquiv(loopSrc, "<loop_src>", loopMod, errorOut) ||
        !lowerSingleSourceForEquiv(closedFormSrc, "<closed_form_src>", closedMod, errorOut)) {
        out.detail = errorOut ? *errorOut : "failed to lower source";
        return out;
    }
    auto findTargetFn = [](const il::Module& mod) -> const il::Function* {
        for (const auto& fn : mod.functions) {
            if (!fn.isExternal && !fn.isEntryPoint && fn.name != "main" &&
                fn.name != "bronze_main" && fn.name != "__main__" && fn.name != "<module>" && !fn.blocks.empty()) {
                return &fn;
            }
        }
        return nullptr;
    };
    const il::Function* loopFn = findTargetFn(loopMod);
    const il::Function* closedFn = findTargetFn(closedMod);
    if (!loopFn || !closedFn) {
        out.detail = "no comparable top-level function found";
        if (errorOut) *errorOut = out.detail;
        return out;
    }
    FunctionEquivResult fnRes = compareIlLoopFunctionPairInductive(
        loopMod, *loopFn, closedMod, *closedFn, true, domainMin, domainMax);
    out.status = fnRes.status;
    out.fnName = fnRes.name;
    out.detail = fnRes.detail;
    out.functions.push_back(fnRes);
    if (fnRes.status == EquivStatus::Counterexample) {
        out.counterexampleArgs = fnRes.counterexampleArgs;
        if (!fnRes.counterexampleArgs.empty()) out.counterexampleInput = fnRes.counterexampleArgs[0];
        out.expectedOutput = fnRes.oldReturnValue;
        out.actualOutput = fnRes.newReturnValue;
    }
    return out;
}

}  // namespace bronze::check
