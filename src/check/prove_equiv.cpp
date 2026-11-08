#include "check/prove_equiv.h"
#include "check/prove_equiv_heap.h"
#include "check/il_interp.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <verify/bv_solver.hpp>

#include "lex/lexer.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

const std::string& effectiveFnName(const il::Function& fn) {
    return !fn.displayName.empty() ? fn.displayName : fn.name;
}

bool functionMatchesName(const il::Function& fn, std::string_view query) {
    if (fn.name == query || fn.displayName == query) return true;
    auto pos = fn.name.rfind("::");
    if (pos != std::string::npos && std::string_view(fn.name).substr(pos + 2) == query) {
        return true;
    }
    if (!fn.displayName.empty()) {
        auto dot = fn.displayName.rfind('.');
        if (dot != std::string::npos && std::string_view(fn.displayName).substr(dot + 1) == query) {
            return true;
        }
    }
    return false;
}

bool ilFunctionsIdentical(const il::Module& modA, const il::Function& a,
                          const il::Module& modB, const il::Function& b) {
    if (a.blocks.size() != b.blocks.size() || a.params.size() != b.params.size()) return false;
    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        const auto& ba = a.blocks[bi];
        const auto& bb = b.blocks[bi];
        if (ba.instructions.size() != bb.instructions.size()) return false;
        for (size_t ii = 0; ii < ba.instructions.size(); ++ii) {
            const auto& ia = ba.instructions[ii];
            const auto& ib = bb.instructions[ii];
            if (ia.op != ib.op || ia.result != ib.result || ia.operands != ib.operands ||
                ia.immI32 != ib.immI32 || ia.immF64 != ib.immF64 ||
                ia.target.block != ib.target.block || ia.elseTarget.block != ib.elseTarget.block) {
                return false;
            }
            if (ia.keyIndex != ib.keyIndex) {
                std::string_view ka = (ia.keyIndex < modA.keyConstants.size())
                                          ? std::string_view(modA.keyConstants[ia.keyIndex])
                                          : std::string_view{};
                std::string_view kb = (ib.keyIndex < modB.keyConstants.size())
                                          ? std::string_view(modB.keyConstants[ib.keyIndex])
                                          : std::string_view{};
                if (ka != kb) return false;
            }
        }
    }
    return true;
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

bool lowerFileToModule(const std::string& path,
                       const std::vector<modules::ModuleRoot>& moduleRoots,
                       const std::string& importMapPath,
                       il::Module& outMod,
                       std::string& errorMsg) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astMod = modules::loadProgram(path, sources, diags, {moduleRoots, importMapPath});
    if (diags.hasErrors() || !astMod) {
        errorMsg = diags.render(sources);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        errorMsg = diags.render(sources);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        errorMsg = diags.render(sources);
        return false;
    }
    outMod = std::move(*lowered);
    return true;
}

}  // namespace

bool lowerSingleSourceForEquiv(std::string_view src,
                               std::string_view virtualName,
                               il::Module& outMod,
                               std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add(std::string(virtualName), std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto astMod = Parser(std::move(tokens), diags).parseModule(std::string(virtualName));
    if (diags.hasErrors() || !astMod) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    outMod = std::move(*lowered);
    return true;
}

const char* equivStatusTag(EquivStatus status) {
    switch (status) {
        case EquivStatus::Equivalent: return "EQUIVALENT";
        case EquivStatus::Bounded: return "BOUNDED";
        case EquivStatus::Counterexample: return "COUNTEREXAMPLE";
        case EquivStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool ProveEquivReport::allEquivalent() const {
    if (functions.empty()) return false;
    for (const auto& f : functions) {
        if (f.status != EquivStatus::Equivalent) return false;
    }
    return true;
}

bool ProveEquivReport::hasCounterexample() const {
    for (const auto& f : functions) {
        if (f.status == EquivStatus::Counterexample) return true;
    }
    return false;
}

std::string ProveEquivReport::renderText() const {
    std::ostringstream os;
    for (const auto& f : functions) {
        os << f.name << ": " << equivStatusTag(f.status);
        if (f.status == EquivStatus::Bounded) {
            os << " (";
            bool first = true;
            if (f.bounds.maxLoopIterations > 0) {
                os << "loop <= " << f.bounds.maxLoopIterations;
                first = false;
            }
            if (f.bounds.maxArrayLength > 0) {
                if (!first) os << ", ";
                os << "arrayLen <= " << f.bounds.maxArrayLength;
                first = false;
            }
            if (f.bounds.maxStringLength > 0) {
                if (!first) os << ", ";
                os << "stringLen <= " << f.bounds.maxStringLength;
                first = false;
            }
            if (!f.detail.empty()) {
                if (!first) os << "; ";
                os << f.detail;
            }
            os << ")";
        } else if (f.status == EquivStatus::Counterexample) {
            os << " (";
            for (size_t i = 0; i < f.counterexampleArgs.size(); ++i) {
                if (i > 0) os << ", ";
                std::string pname = (i < f.paramNames.size()) ? f.paramNames[i] : ("arg_" + std::to_string(i));
                os << pname << " = " << f.counterexampleArgs[i];
            }
            os << ") -> old = " << (f.oldThrows ? std::string("throws") : std::to_string(f.oldReturnValue))
               << ", new = " << (f.newThrows ? std::string("throws") : std::to_string(f.newReturnValue));
        } else if (f.status == EquivStatus::Unknown && !f.detail.empty()) {
            os << " (" << f.detail << ")";
        }
        os << "\n";
    }
    return os.str();
}

std::string ProveEquivReport::renderJson() const {
    std::ostringstream os;
    os << "{\"functions\":[";
    for (size_t i = 0; i < functions.size(); ++i) {
        if (i > 0) os << ",";
        const auto& f = functions[i];
        os << "{\"name\":\"" << jsonEscape(f.name) << "\","
           << "\"status\":\"" << equivStatusTag(f.status) << "\","
           << "\"detail\":\"" << jsonEscape(f.detail) << "\"";
        if (f.status == EquivStatus::Bounded || f.bounds.hasBounds()) {
            os << ",\"bounds\":{"
               << "\"loopTripCount\":" << f.bounds.maxLoopIterations << ","
               << "\"arrayLength\":" << f.bounds.maxArrayLength << ","
               << "\"stringLength\":" << f.bounds.maxStringLength << "}";
        }
        if (f.status == EquivStatus::Counterexample) {
            os << ",\"counterexample\":{\"params\":[";
            for (size_t j = 0; j < f.paramNames.size(); ++j) {
                if (j > 0) os << ",";
                os << "\"" << jsonEscape(f.paramNames[j]) << "\"";
            }
            os << "],\"args\":[";
            for (size_t j = 0; j < f.counterexampleArgs.size(); ++j) {
                if (j > 0) os << ",";
                os << f.counterexampleArgs[j];
            }
            os << "],\"oldReturn\":" << f.oldReturnValue
               << ",\"newReturn\":" << f.newReturnValue
               << ",\"oldThrows\":" << (f.oldThrows ? "true" : "false")
               << ",\"newThrows\":" << (f.newThrows ? "true" : "false") << "}";
        }
        os << "}";
    }
    os << "],\"allEquivalent\":" << (allEquivalent() ? "true" : "false") << "}\n";
    return os.str();
}

FunctionEquivResult compareIlFunctionPair(const il::Module& oldMod,
                                          const il::Function& oldFn,
                                          const il::Module& newMod,
                                          const il::Function& newFn,
                                          bool strictJs32Bitwise,
                                          int32_t domainMin,
                                          int32_t domainMax) {
    using brass::verify::BVContext;
    using brass::verify::BVNodeId;
    using brass::verify::BVOp;
    using brass::verify::SolveOutcome;

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
        if (pname.empty()) pname = "arg_" + std::to_string(i);
        res.paramNames.push_back(pname);
    }

    const bool floatMode = (domainMin == INT32_MIN && domainMax == INT32_MAX) &&
                           (functionRequiresFloatMode(oldMod, oldFn) || functionRequiresFloatMode(newMod, newFn));

    BVContext ctx;
    std::vector<BVNodeId> argNodes;
    argNodes.reserve(numParams);
    for (uint32_t i = 0; i < numParams; ++i) {
        if (floatMode) {
            argNodes.push_back(ctx.make_var("arg_" + std::to_string(i), 64, true));
        } else {
            BVNodeId raw32 = ctx.make_var("arg_" + std::to_string(i), 32);
            argNodes.push_back(ctx.make_sext(raw32, 32));
        }
    }

    SymbolicHeapTable heapTable(ctx, floatMode);
    BVNodeId oldRetNode = 0;
    BVNodeId newRetNode = 0;
    BVNodeId oldThrewNode = 0;
    BVNodeId newThrewNode = 0;
    BVNodeId oldExceededNode = 0;
    BVNodeId newExceededNode = 0;
    ProofBounds oldBounds;
    ProofBounds newBounds;
    std::string oldReason;
    std::string newReason;
    SymbolicHeapState oldFinalHeap;
    SymbolicHeapState newFinalHeap;
    bool oldHitUnroll = false;
    bool newHitUnroll = false;

    bool oldOk = evalIlFunctionSymbolic(ctx, oldMod, oldFn, argNodes, strictJs32Bitwise,
                                        &oldRetNode, &oldReason, &heapTable, &oldFinalHeap,
                                        &oldHitUnroll, &oldThrewNode, &oldExceededNode, &oldBounds);
    bool newOk = evalIlFunctionSymbolic(ctx, newMod, newFn, argNodes, strictJs32Bitwise,
                                        &newRetNode, &newReason, &heapTable, &newFinalHeap,
                                        &newHitUnroll, &newThrewNode, &newExceededNode, &newBounds);

    if (!oldOk || !newOk || oldHitUnroll || newHitUnroll) {
        auto indRes = compareIlLoopFunctionPairInductive(
            oldMod, oldFn, newMod, newFn, strictJs32Bitwise, domainMin, domainMax);
        if (indRes.status != EquivStatus::Unknown) {
            return indRes;
        }
        if (!oldOk) {
            res.status = EquivStatus::Unknown;
            res.detail = "old function not symbolically decidable: " + oldReason;
            return res;
        }
        if (!newOk) {
            res.status = EquivStatus::Unknown;
            res.detail = "new function not symbolically decidable: " + newReason;
            return res;
        }
    }

    ProofBounds combinedBounds;
    combinedBounds.maxLoopIterations = std::max(oldBounds.maxLoopIterations, newBounds.maxLoopIterations);
    if (oldBounds.maxLoopIterations > 0 && newBounds.maxLoopIterations > 0) {
        combinedBounds.maxLoopIterations = std::min(oldBounds.maxLoopIterations, newBounds.maxLoopIterations);
    }
    combinedBounds.maxArrayLength = std::max(oldBounds.maxArrayLength, newBounds.maxArrayLength);
    combinedBounds.maxStringLength = std::max(oldBounds.maxStringLength, newBounds.maxStringLength);
    if ((oldHitUnroll || newHitUnroll) && combinedBounds.maxLoopIterations == 0) {
        combinedBounds.maxLoopIterations = 8;
    }
    const bool isBounded = oldHitUnroll || newHitUnroll || combinedBounds.hasBounds();

    BVNodeId zero1 = ctx.make_const(0, 1);
    BVNodeId oExc = (oldExceededNode != 0) ? oldExceededNode : zero1;
    BVNodeId nExc = (newExceededNode != 0) ? newExceededNode : zero1;
    BVNodeId neitherExceeded = ctx.make_unary(BVOp::Not, ctx.make_binary(BVOp::Or, oExc, nExc));

    auto applyDomainConstraints = [&](BVNodeId cond) {
        if (domainMin > INT32_MIN || domainMax < INT32_MAX) {
            BVNodeId minNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMin)), 64);
            BVNodeId maxNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMax)), 64);
            auto constrainNode = [&](BVNodeId n) {
                if (domainMin > INT32_MIN) {
                    cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sge, n, minNode));
                }
                if (domainMax < INT32_MAX) {
                    cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sle, n, maxNode));
                }
            };
            for (BVNodeId arg : argNodes) constrainNode(arg);
            for (BVNodeId hv : heapTable.varNodes()) constrainNode(hv);
        }
        return cond;
    };

    auto finalizeNoCounterexample = [&](std::string unbDetail) -> FunctionEquivResult {
        if (!isBounded) {
            res.status = EquivStatus::Equivalent;
            res.detail = std::move(unbDetail);
            return res;
        }
        if (oExc != nExc) {
            BVNodeId excDiff = applyDomainConstraints(ctx.make_binary(BVOp::Ne, oExc, nExc));
            SolveOutcome excSat = brass::verify::solve_nonzero(ctx, excDiff);
            if (excSat.status == SolveOutcome::Sat) {
                const size_t totalVars = numParams + heapTable.varNodes().size();
                std::vector<uint64_t> candModel = excSat.model;
                if (candModel.size() < totalVars) candModel.resize(totalVars, 0);
                if (floatMode) {
                    minimizeFloatCounterexampleModel(ctx, excDiff, totalVars, candModel);
                } else {
                    minimizeCounterexampleModel(ctx, excDiff, totalVars, domainMin, domainMax, candModel);
                }
                std::vector<std::string> candNames = res.paramNames;
                std::vector<int64_t> candArgs(numParams, 0);
                for (uint32_t i = 0; i < numParams; ++i) {
                    candArgs[i] = floatMode ? static_cast<int64_t>(candModel[i])
                                            : static_cast<int64_t>(static_cast<int32_t>(candModel[i]));
                }
                for (size_t i = 0; i < heapTable.varNodes().size(); ++i) {
                    candNames.push_back(heapTable.varPaths()[i]);
                    candArgs.push_back(floatMode ? static_cast<int64_t>(candModel[numParams + i])
                                                 : static_cast<int64_t>(static_cast<int32_t>(candModel[numParams + i])));
                }
                InterpResult oRun = runIlFunctionConcrete(oldMod, oldFn, candNames, candArgs, 50000, floatMode);
                InterpResult nRun = runIlFunctionConcrete(newMod, newFn, candNames, candArgs, 50000, floatMode);
                int64_t oVal = 0, nVal = 0;
                if (oRun.completed && nRun.completed && concreteResultsDiverge(oRun, nRun, &oVal, &nVal)) {
                    res.status = EquivStatus::Counterexample;
                    res.paramNames = std::move(candNames);
                    res.counterexampleArgs = std::move(candArgs);
                    res.oldReturnValue = oVal;
                    res.newReturnValue = nVal;
                    res.oldThrows = oRun.threw;
                    res.newThrows = nRun.threw;
                    res.detail = "counterexample found where loop trip count or return value diverges";
                    return res;
                }
            }
        }
        BVNodeId inBoundsCond = applyDomainConstraints(neitherExceeded);
        if (brass::verify::solve_nonzero(ctx, inBoundsCond).status != SolveOutcome::Sat) {
            res.status = EquivStatus::Unknown;
            res.detail = "loop minimum trip count exceeds bounded unroll limit";
            return res;
        }
        res.status = EquivStatus::Bounded;
        res.bounds = combinedBounds;
        res.detail = "proven equivalent for all inputs up to stated bounds";
        return res;
    };

    std::vector<std::string> checkedHeapPaths;
    BVNodeId heapMismatch = SymbolicHeapState::buildHeapMismatchCond(
        heapTable, oldFinalHeap, newFinalHeap, &checkedHeapPaths);

    if (oldRetNode == newRetNode && heapMismatch == 0 && oldThrewNode == newThrewNode) {
        return finalizeNoCounterexample("proven equivalent by structural BV hash-consing");
    }

    BVNodeId valMismatch = 0;
    if (oldRetNode != newRetNode) {
        if (floatMode) {
            BVNodeId oldNaN = ctx.make_binary(BVOp::FNe, oldRetNode, oldRetNode, 1);
            BVNodeId newNaN = ctx.make_binary(BVOp::FNe, newRetNode, newRetNode, 1);
            BVNodeId bothNaN = ctx.make_binary(BVOp::And, oldNaN, newNaN, 1);
            BVNodeId bitEq = ctx.make_binary(BVOp::Eq, oldRetNode, newRetNode, 1);
            valMismatch = ctx.make_unary(BVOp::Not, ctx.make_binary(BVOp::Or, bothNaN, bitEq, 1), 1);
        } else {
            valMismatch = ctx.make_unary(BVOp::Not, ctx.make_binary(BVOp::Eq, oldRetNode, newRetNode));
        }
    }
    if (heapMismatch != 0) {
        valMismatch = (valMismatch == 0) ? heapMismatch : ctx.make_binary(BVOp::Or, valMismatch, heapMismatch);
    }

    BVNodeId threwDiff = (oldThrewNode != newThrewNode)
                             ? ctx.make_binary(BVOp::Ne, oldThrewNode, newThrewNode) : 0;
    BVNodeId neitherThrew = ctx.make_unary(BVOp::Not, ctx.make_binary(BVOp::Or, oldThrewNode, newThrewNode));
    BVNodeId badCond = (valMismatch != 0) ? ctx.make_binary(BVOp::And, neitherThrew, valMismatch) : 0;
    if (threwDiff != 0) {
        badCond = (badCond == 0) ? threwDiff : ctx.make_binary(BVOp::Or, threwDiff, badCond);
    }
    if (isBounded) {
        badCond = ctx.make_binary(BVOp::And, badCond, neitherExceeded);
    }

    BVNodeId aliasCongruence = heapTable.buildAliasCongruenceCond();
    if (aliasCongruence != 0) {
        badCond = ctx.make_binary(BVOp::And, badCond, aliasCongruence);
    }
    badCond = applyDomainConstraints(badCond);

    SolveOutcome satRes = brass::verify::solve_nonzero(ctx, badCond);

    if (satRes.status == SolveOutcome::Unsat) {
        return finalizeNoCounterexample(
            "proven equivalent across all symbolic inputs and post-states via CDCL bit-blasting");
    }

    if (satRes.status == SolveOutcome::Sat) {
        res.status = EquivStatus::Counterexample;
        const size_t totalVars = numParams + heapTable.varNodes().size();
        std::vector<uint64_t> bestModel = satRes.model;
        if (bestModel.size() < totalVars) bestModel.resize(totalVars, 0);
        if (floatMode) {
            minimizeFloatCounterexampleModel(ctx, badCond, totalVars, bestModel);
        } else {
            minimizeCounterexampleModel(ctx, badCond, totalVars, domainMin, domainMax, bestModel);
            for (size_t i = 0; i < totalVars; ++i) {
                int32_t signed32 = static_cast<int32_t>(static_cast<uint32_t>(bestModel[i]));
                bestModel[i] = static_cast<uint64_t>(static_cast<uint32_t>(signed32));
            }
        }
        res.counterexampleArgs.resize(numParams, 0);
        for (uint32_t i = 0; i < numParams; ++i) {
            res.counterexampleArgs[i] = floatMode
                ? static_cast<int64_t>(bestModel[i])
                : static_cast<int64_t>(static_cast<int32_t>(bestModel[i]));
        }
        if (!heapTable.varNodes().empty()) {
            for (size_t i = 0; i < heapTable.varNodes().size(); ++i) {
                res.paramNames.push_back(heapTable.varPaths()[i]);
                res.counterexampleArgs.push_back(
                    floatMode ? static_cast<int64_t>(bestModel[numParams + i])
                              : static_cast<int64_t>(static_cast<int32_t>(bestModel[numParams + i])));
            }
        }
        int64_t oldRet = static_cast<int64_t>(ctx.eval_concrete(oldRetNode, bestModel));
        int64_t newRet = static_cast<int64_t>(ctx.eval_concrete(newRetNode, bestModel));
        if (oldRet == newRet && !checkedHeapPaths.empty()) {
            for (const std::string& p : checkedHeapPaths) {
                int64_t ov = static_cast<int64_t>(
                    ctx.eval_concrete(oldFinalHeap.getFinalValue(heapTable, p), bestModel));
                int64_t nv = static_cast<int64_t>(
                    ctx.eval_concrete(newFinalHeap.getFinalValue(heapTable, p), bestModel));
                if (ov != nv) {
                    oldRet = ov;
                    newRet = nv;
                    res.detail = "counterexample found where post-state property `" + p + "` diverges";
                    break;
                }
            }
        }
        res.oldReturnValue = oldRet;
        res.newReturnValue = newRet;
        res.oldThrows = (ctx.eval_concrete(oldThrewNode, bestModel) != 0);
        res.newThrows = (ctx.eval_concrete(newThrewNode, bestModel) != 0);
        if (res.detail.empty()) {
            res.detail = "counterexample found where old and new return values diverge";
        }
        verifyCounterexampleWithReplay(oldMod, oldFn, newMod, newFn, res, floatMode);
        return res;
    }

    res.status = EquivStatus::Unknown;
    res.detail = "solver returned UNKNOWN (unsupported non-linear op or resource bound)";
    return res;
}

ProveEquivReport proveModuleEquiv(const il::Module& oldMod,
                                  const il::Module& newMod,
                                  const std::string& fnFilter) {
    ProveEquivReport report;

    if (!fnFilter.empty()) {
        const il::Function* oldFn = nullptr;
        const il::Function* newFn = nullptr;
        for (const auto& f : oldMod.functions) {
            if (!f.isExternal && functionMatchesName(f, fnFilter)) {
                oldFn = &f;
                break;
            }
        }
        for (const auto& f : newMod.functions) {
            if (!f.isExternal && functionMatchesName(f, fnFilter)) {
                newFn = &f;
                break;
            }
        }
        if (!oldFn || !newFn) {
            FunctionEquivResult missing;
            missing.name = fnFilter;
            missing.status = EquivStatus::Unknown;
            missing.detail = !oldFn ? "function not found in old module"
                                    : "function not found in new module";
            report.functions.push_back(std::move(missing));
            return report;
        }
        report.functions.push_back(compareIlFunctionPair(oldMod, *oldFn, newMod, *newFn));
        return report;
    }

    for (const auto& oldFn : oldMod.functions) {
        if (oldFn.isExternal || oldFn.isEntryPoint || oldFn.name == "main") continue;
        const std::string& oldKey = effectiveFnName(oldFn);
        const il::Function* matchedNew = nullptr;
        for (const auto& newFn : newMod.functions) {
            if (newFn.isExternal || newFn.isEntryPoint || newFn.name == "main") continue;
            if (effectiveFnName(newFn) == oldKey ||
                functionMatchesName(newFn, oldKey) ||
                functionMatchesName(oldFn, effectiveFnName(newFn))) {
                matchedNew = &newFn;
                break;
            }
        }
        if (matchedNew) {
            report.functions.push_back(compareIlFunctionPair(oldMod, oldFn, newMod, *matchedNew));
        }
    }

    if (report.functions.empty()) {
        const il::Function* oldMain = nullptr;
        const il::Function* newMain = nullptr;
        for (const auto& f : oldMod.functions) {
            if (f.isEntryPoint || f.name == "main") { oldMain = &f; break; }
        }
        for (const auto& f : newMod.functions) {
            if (f.isEntryPoint || f.name == "main") { newMain = &f; break; }
        }
        if (oldMain && newMain) {
            report.functions.push_back(compareIlFunctionPair(oldMod, *oldMain, newMod, *newMain));
        }
    }

    return report;
}

ProveEquivReport proveSourceEquiv(std::string_view oldSrc,
                                  std::string_view newSrc,
                                  const std::string& fnFilter,
                                  std::string* errorOut) {
    il::Module oldMod;
    il::Module newMod;
    if (!lowerSingleSourceForEquiv(oldSrc, "old.js", oldMod, errorOut)) {
        return {};
    }
    if (!lowerSingleSourceForEquiv(newSrc, "new.js", newMod, errorOut)) {
        return {};
    }
    return proveModuleEquiv(oldMod, newMod, fnFilter);
}

EquivReport proveSourceEquivalent(std::string_view oldSrc, std::string_view newSrc,
                                  std::string_view fnFilter, int32_t domainMin,
                                  int32_t domainMax, std::string* errorOut) {
    EquivReport rep;
    rep.domainMin = domainMin;
    rep.domainMax = domainMax;
    il::Module oldMod, newMod;
    if (!lowerSingleSourceForEquiv(oldSrc, "old.js", oldMod, errorOut) ||
        !lowerSingleSourceForEquiv(newSrc, "new.js", newMod, errorOut)) {
        rep.status = EquivStatus::Unknown;
        rep.detail = errorOut ? *errorOut : "failed to lower source";
        return rep;
    }

    auto populateReportFromFnResult = [&](FunctionEquivResult fnRes) {
        rep.status = fnRes.status;
        rep.fnName = fnRes.name;
        rep.detail = fnRes.detail;
        rep.bounds = fnRes.bounds;
        if (!fnRes.counterexampleArgs.empty()) {
            rep.counterexampleInput = fnRes.counterexampleArgs[0];
            rep.counterexampleArgs = fnRes.counterexampleArgs;
        }
        rep.expectedOutput = fnRes.oldReturnValue;
        rep.actualOutput = fnRes.newReturnValue;
        rep.functions.push_back(std::move(fnRes));
    };

    if (!fnFilter.empty()) {
        const il::Function* oldFn = nullptr;
        const il::Function* newFn = nullptr;
        for (const auto& f : oldMod.functions) {
            if (!f.isExternal && !f.isEntryPoint && f.name != "main" && functionMatchesName(f, fnFilter)) {
                oldFn = &f;
                break;
            }
        }
        for (const auto& f : newMod.functions) {
            if (!f.isExternal && !f.isEntryPoint && f.name != "main" && functionMatchesName(f, fnFilter)) {
                newFn = &f;
                break;
            }
        }
        if (!oldFn || !newFn) {
            rep.status = EquivStatus::Unknown;
            rep.detail = "target function not found";
            return rep;
        }
        populateReportFromFnResult(
            compareIlFunctionPair(oldMod, *oldFn, newMod, *newFn, true, domainMin, domainMax));
        return rep;
    }

    // Collect all matched non-entry functions/methods.
    struct FnPair {
        const il::Function* oldFn = nullptr;
        const il::Function* newFn = nullptr;
        bool modified = false;
    };
    std::vector<FnPair> allPairs;
    for (const auto& oldFn : oldMod.functions) {
        if (oldFn.isExternal || oldFn.isEntryPoint || oldFn.name == "main") continue;
        const std::string& oldKey = effectiveFnName(oldFn);
        for (const auto& newFn : newMod.functions) {
            if (newFn.isExternal || newFn.isEntryPoint || newFn.name == "main") continue;
            if (effectiveFnName(newFn) == oldKey ||
                functionMatchesName(newFn, oldKey) ||
                functionMatchesName(oldFn, effectiveFnName(newFn))) {
                bool same = ilFunctionsIdentical(oldMod, oldFn, newMod, newFn);
                allPairs.push_back({&oldFn, &newFn, !same});
                break;
            }
        }
    }
    if (allPairs.empty()) {
        const il::Function* oldFn = nullptr;
        const il::Function* newFn = nullptr;
        for (const auto& f : oldMod.functions) {
            if (!f.isExternal && !f.isEntryPoint && f.name != "main") { oldFn = &f; break; }
        }
        for (const auto& f : newMod.functions) {
            if (!f.isExternal && !f.isEntryPoint && f.name != "main") { newFn = &f; break; }
        }
        if (!oldFn || !newFn) {
            rep.status = EquivStatus::Unknown;
            rep.detail = "target function not found";
            return rep;
        }
        allPairs.push_back({oldFn, newFn, true});
    }

    std::vector<FnPair> targetPairs;
    for (const auto& p : allPairs) {
        if (p.modified) targetPairs.push_back(p);
    }
    if (targetPairs.empty()) {
        // No function changed at IL level: prefer methods (containing `.`) over synthetic constructors
        for (const auto& p : allPairs) {
            if (effectiveFnName(*p.oldFn).find('.') != std::string::npos) {
                targetPairs.push_back(p);
            }
        }
        if (targetPairs.empty()) targetPairs = allPairs;
    }

    for (const auto& p : targetPairs) {
        FunctionEquivResult fnRes = compareIlFunctionPair(
            oldMod, *p.oldFn, newMod, *p.newFn, true, domainMin, domainMax);
        if (fnRes.status != EquivStatus::Equivalent) {
            populateReportFromFnResult(std::move(fnRes));
            return rep;
        }
        if (rep.functions.empty()) {
            rep.status = fnRes.status;
            rep.fnName = fnRes.name;
            rep.detail = fnRes.detail;
        }
        rep.functions.push_back(std::move(fnRes));
    }
    return rep;
}

EquivReport proveRoundTripSource(std::string_view source, std::string_view encodeFnName,
                                 std::string_view decodeFnName, int32_t domainMin,
                                 int32_t domainMax, std::string* errorOut) {
    using brass::verify::BVContext;
    using brass::verify::BVNodeId;
    using brass::verify::BVOp;
    using brass::verify::SolveOutcome;

    EquivReport rep;
    rep.fnName = std::string(decodeFnName) + "(" + std::string(encodeFnName) + "(x))";
    rep.domainMin = domainMin;
    rep.domainMax = domainMax;
    il::Module mod;
    if (!lowerSingleSourceForEquiv(source, "roundtrip.js", mod, errorOut)) {
        rep.status = EquivStatus::Unknown;
        rep.detail = errorOut ? *errorOut : "failed to lower source";
        return rep;
    }
    const il::Function* encFn = nullptr;
    const il::Function* decFn = nullptr;
    for (const auto& f : mod.functions) {
        if (f.isExternal) continue;
        if (!encFn && functionMatchesName(f, encodeFnName)) encFn = &f;
        if (!decFn && functionMatchesName(f, decodeFnName)) decFn = &f;
    }
    if (!encFn || !decFn) {
        rep.status = EquivStatus::Unknown;
        rep.detail = !encFn ? "encode function not found" : "decode function not found";
        return rep;
    }
    BVContext ctx;
    BVNodeId raw32 = ctx.make_var("x", 32);
    BVNodeId xNode = ctx.make_sext(raw32, 32);
    BVNodeId encRet = 0, decRet = 0;
    BVNodeId encThrew = 0, decThrew = 0;
    std::string reason;
    if (!evalIlFunctionSymbolic(ctx, mod, *encFn, {xNode}, true, &encRet, &reason, nullptr, nullptr, nullptr, &encThrew)) {
        rep.status = EquivStatus::Unknown;
        rep.detail = "encode function not symbolically decidable: " + reason;
        return rep;
    }
    if (!evalIlFunctionSymbolic(ctx, mod, *decFn, {encRet}, true, &decRet, &reason, nullptr, nullptr, nullptr, &decThrew)) {
        rep.status = EquivStatus::Unknown;
        rep.detail = "decode function not symbolically decidable: " + reason;
        return rep;
    }
    BVNodeId badCond = ctx.make_binary(BVOp::Or, decThrew, ctx.make_binary(BVOp::Ne, decRet, xNode));
    // An x encode throws on is outside its domain; decode throwing on what encode
    // produced is a round-trip failure, so only the encode side is excluded.
    badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_unary(BVOp::Not, encThrew));
    if (domainMin > INT32_MIN) {
        BVNodeId minNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMin)), 64);
        badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sge, xNode, minNode));
    }
    if (domainMax < INT32_MAX) {
        BVNodeId maxNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(domainMax)), 64);
        badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sle, xNode, maxNode));
    }
    SolveOutcome satRes = brass::verify::solve_nonzero(ctx, badCond);
    if (satRes.status == SolveOutcome::Unsat) {
        rep.status = EquivStatus::ProvenEquivalent;
        rep.detail = "proven decode(encode(x)) === x across all symbolic inputs in domain";
        FunctionEquivResult fnRes{rep.fnName, {"x"}, EquivStatus::ProvenEquivalent, {}, 0, 0, rep.detail};
        rep.functions.push_back(std::move(fnRes));
        return rep;
    }
    if (satRes.status == SolveOutcome::Sat) {
        rep.status = EquivStatus::RefutedWithCounterexample;
        std::vector<uint64_t> bestModel = satRes.model.empty() ? std::vector<uint64_t>{0} : satRes.model;
        static constexpr int32_t kBoundaryCandidates[] = {
            0, 1, -1, 2, -2, 63, 64, 127, 128, -128, 255, 256,
            1073741823, 1073741824, -1073741823, -1073741824, INT32_MAX, INT32_MIN
        };
        for (int32_t c : kBoundaryCandidates) {
            if (c < domainMin || c > domainMax) continue;
            std::vector<uint64_t> cand{static_cast<uint64_t>(static_cast<uint32_t>(c))};
            if (ctx.eval_concrete(badCond, cand) != 0) {
                bestModel = std::move(cand);
                break;
            }
        }
        int32_t signed32 = static_cast<int32_t>(static_cast<uint32_t>(bestModel[0]));
        bestModel[0] = static_cast<uint64_t>(static_cast<uint32_t>(signed32));
        rep.counterexampleInput = static_cast<int64_t>(signed32);
        rep.counterexampleArgs = {rep.counterexampleInput};
        rep.expectedOutput = rep.counterexampleInput;
        rep.encodedIntermediate = static_cast<int64_t>(ctx.eval_concrete(encRet, bestModel));
        rep.actualOutput = static_cast<int64_t>(ctx.eval_concrete(decRet, bestModel));
        rep.detail = "refuted round-trip: found input x where decode(encode(x)) !== x";
        FunctionEquivResult fnRes{rep.fnName, {"x"}, EquivStatus::RefutedWithCounterexample,
                                  rep.counterexampleArgs, rep.expectedOutput, rep.actualOutput, rep.detail};
        rep.functions.push_back(std::move(fnRes));
        return rep;
    }
    rep.status = EquivStatus::Unknown;
    rep.detail = "solver returned UNKNOWN on round-trip condition";
    return rep;
}

int runProveEquiv(const std::string& oldPath,
                  const std::string& newPath,
                  const std::string& fnFilter,
                  bool jsonOutput,
                  std::ostream& out,
                  std::ostream& err,
                  const std::vector<modules::ModuleRoot>& moduleRoots,
                  const std::string& importMapPath) {
    il::Module oldMod;
    il::Module newMod;
    std::string errMsg;
    if (!lowerFileToModule(oldPath, moduleRoots, importMapPath, oldMod, errMsg)) {
        err << errMsg;
        return 1;
    }
    if (!lowerFileToModule(newPath, moduleRoots, importMapPath, newMod, errMsg)) {
        err << errMsg;
        return 1;
    }

    ProveEquivReport report = proveModuleEquiv(oldMod, newMod, fnFilter);
    if (report.empty()) {
        err << "error: no matching functions found to compare between " << oldPath << " and " << newPath << "\n";
        return 1;
    }

    if (jsonOutput) {
        out << report.renderJson();
    } else {
        out << report.renderText();
    }

    if (report.hasCounterexample()) return 1;
    if (!report.allEquivalent()) return 2;
    return 0;
}

}  // namespace bronze::check
