#include "check/package_audit.h"
#include "check/il_interp.h"
#include "check/prove_equiv_heap.h"
#include "check/prove_equiv_sym.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>

#include "lower/lower.h"
#include "modules/modules.h"
#include "support/diagnostics.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

std::string effectiveAuditFnName(const il::Function& fn) {
    return !fn.displayName.empty() ? fn.displayName : fn.name;
}

std::string shortMethodName(std::string_view fullName) {
    auto dot = fullName.rfind('.');
    return (dot == std::string_view::npos) ? std::string(fullName) : std::string(fullName.substr(dot + 1));
}

std::string ownerPrefixName(std::string_view fullName) {
    auto dot = fullName.rfind('.');
    return (dot == std::string_view::npos) ? std::string{} : std::string(fullName.substr(0, dot));
}

std::string toLowerCopy(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return out;
}

bool isCandidatePublicFunction(const il::Function& fn) {
    if (fn.isExternal || fn.isEntryPoint || fn.blocks.empty()) return false;
    std::string name = effectiveAuditFnName(fn);
    if (name.empty() || name == "main" || name == "constructor") return false;
    if (name.find("anon") != std::string::npos || name.find('$') != std::string::npos) return false;
    std::string shortName = shortMethodName(name);
    if (shortName.empty() || shortName[0] == '_') return false;
    return true;
}

bool lowerEntryFileForAudit(const std::string& path,
                            const std::vector<modules::ModuleRoot>& moduleRoots,
                            const std::string& importMapPath,
                            il::Module& outMod,
                            std::string& outErr) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astMod = modules::loadProgram(path, sources, diags, {moduleRoots, importMapPath});
    if (!astMod) {
        outErr = diags.render(sources);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        outErr = diags.render(sources);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        outErr = diags.render(sources);
        return false;
    }
    outMod = std::move(*lowered);
    for (const auto& fn : outMod.functions) {
        std::unordered_map<il::ValueId, uint32_t> fnValToCallee;
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if ((inst.op == il::Op::CreateFunction || inst.op == il::Op::FunctionRef) &&
                    inst.result != il::kNoValue && inst.calleeIndex < outMod.functions.size()) {
                    fnValToCallee[inst.result] = inst.calleeIndex;
                } else if ((inst.op == il::Op::Box || inst.op == il::Op::Unbox) &&
                           !inst.operands.empty() && inst.result != il::kNoValue &&
                           fnValToCallee.count(inst.operands[0])) {
                    fnValToCallee[inst.result] = fnValToCallee[inst.operands[0]];
                } else if ((inst.op == il::Op::PropSet || inst.op == il::Op::MethodDef) &&
                           inst.operands.size() >= 2 && inst.keyIndex < outMod.keyConstants.size()) {
                    auto it = fnValToCallee.find(inst.operands[1]);
                    if (it != fnValToCallee.end() && it->second < outMod.functions.size()) {
                        il::Function& target = outMod.functions[it->second];
                        if (target.displayName.empty() || target.displayName.find("anon") != std::string::npos) {
                            target.displayName = outMod.keyConstants[inst.keyIndex];
                        }
                    }
                }
            }
        }
    }
    return true;
}

FunctionDocMeta lookupFunctionDocMeta(const std::map<std::string, FunctionDocMeta>& docMap,
                                      const il::Function& fn) {
    std::string full = effectiveAuditFnName(fn);
    std::string shortN = shortMethodName(full);
    FunctionDocMeta meta;
    auto it = docMap.find(full);
    if (it != docMap.end()) {
        meta = it->second;
    } else {
        auto it2 = docMap.find(shortN);
        if (it2 != docMap.end()) meta = it2->second;
        else meta.name = full;
    }
    if (shortN == "compare" || shortN == "cmp") {
        meta.documentedCompare = true;
        meta.documentedAntisymmetric = true;
    }
    return meta;
}

bool areMorphologicalInverseNames(std::string_view encFull, std::string_view decFull) {
    if (encFull == decFull) return false;
    if (ownerPrefixName(encFull) != ownerPrefixName(decFull)) return false;
    std::string a = shortMethodName(encFull);
    std::string b = shortMethodName(decFull);
    std::string la = toLowerCopy(a);
    std::string lb = toLowerCopy(b);

    // 1. encode<Stem> -> decode<Stem>
    if (la.rfind("encode", 0) == 0 && lb.rfind("decode", 0) == 0 &&
        la.substr(6) == lb.substr(6)) {
        return true;
    }
    // 2. to<Stem> -> from<Stem> (non-empty Stem starting with uppercase or '_')
    if (a.size() > 2 && b.size() > 4 && a.rfind("to", 0) == 0 && b.rfind("from", 0) == 0 &&
        (std::isupper(static_cast<unsigned char>(a[2])) || a[2] == '_') &&
        a.substr(2) == b.substr(4)) {
        return true;
    }
    // 3. <stem> -> un<stem> or de<stem>
    if (la.size() >= 3 && (lb == "un" + la || lb == "de" + la)) {
        return true;
    }
    // 4. <X>To<Y> -> <Y>To<X>
    auto toPosA = a.find("To");
    auto toPosB = b.find("To");
    if (toPosA != std::string::npos && toPosA > 0 && toPosA + 2 < a.size() &&
        toPosB != std::string::npos && toPosB > 0 && toPosB + 2 < b.size()) {
        std::string ax = toLowerCopy(a.substr(0, toPosA));
        std::string ay = toLowerCopy(a.substr(toPosA + 2));
        std::string bx = toLowerCopy(b.substr(0, toPosB));
        std::string by = toLowerCopy(b.substr(toPosB + 2));
        if (ax == by && ay == bx && ax != ay) return true;
    }
    return false;
}

bool areSiblingAgreementNames(std::string_view aFull, std::string_view bFull) {
    if (aFull >= bFull) return false;
    if (ownerPrefixName(aFull) != ownerPrefixName(bFull)) return false;
    std::string a = toLowerCopy(shortMethodName(aFull));
    std::string b = toLowerCopy(shortMethodName(bFull));
    for (const char* sfx : {"sync", "fast", "slow", "fallback", "alt", "ref"}) {
        if (b == a + sfx || a == b + sfx) return true;
    }
    return false;
}

bool areTypesRoundTripCompatible(const FunctionDocMeta& encMeta, const FunctionDocMeta& decMeta) {
    DocValType encIn = !encMeta.params.empty() ? encMeta.params[0].type : DocValType::Unknown;
    DocValType encOut = encMeta.returnType;
    DocValType decIn = !decMeta.params.empty() ? decMeta.params[0].type : DocValType::Unknown;
    DocValType decOut = decMeta.returnType;

    auto compat = [](DocValType x, DocValType y) {
        if (x == DocValType::Unknown || y == DocValType::Unknown ||
            x == DocValType::Any || y == DocValType::Any) {
            return true;
        }
        if ((x == DocValType::Number || x == DocValType::Integer) &&
            (y == DocValType::Number || y == DocValType::Integer)) {
            return true;
        }
        return x == y;
    };
    return compat(encOut, decIn) && compat(decOut, encIn);
}

std::string formatDomainRange(int32_t dMin, int32_t dMax, bool floatMode, bool stringMode) {
    if (stringMode) return "string(len<=8)";
    if (floatMode && dMin == INT32_MIN && dMax == INT32_MAX) return "float64";
    if (dMin == INT32_MIN && dMax == INT32_MAX) return "int32";
    return "[" + std::to_string(dMin) + ", " + std::to_string(dMax) + "]";
}

void recordVerdictCounts(PackageAuditReport& report, const AuditPropertyCheck& chk) {
    if (chk.inPreconditionBucket) {
        report.preconditionBucketCount++;
        return;
    }
    switch (chk.status) {
        case EquivStatus::Equivalent: report.equivalentCount++; break;
        case EquivStatus::Bounded: report.boundedCount++; break;
        case EquivStatus::Counterexample: report.counterexampleCount++; break;
        case EquivStatus::Unknown:
            report.unknownCount++;
            if (!chk.detail.empty()) report.unknownReasons[chk.detail]++;
            break;
    }
}

AuditPropertyCheck checkFunctionDomainSafety(const il::Module& mod,
                                             const il::Function& fn,
                                             const FunctionDocMeta& docMeta,
                                             bool& outAnalyzed) {
    AuditPropertyCheck chk;
    chk.functionName = effectiveAuditFnName(fn);
    chk.propertyKind = "domain-safety";
    int32_t dMin = docMeta.domainMin;
    int32_t dMax = docMeta.domainMax;
    bool floatMode = (dMin == INT32_MIN && dMax == INT32_MAX) && functionRequiresFloatMode(mod, fn);
    bool strMode = functionUsesStringOps(mod, fn) ||
                   (!docMeta.params.empty() && docMeta.params[0].type == DocValType::String);
    std::string domStr = formatDomainRange(dMin, dMax, floatMode, strMode);
    chk.formalProperty = "forall x in " + domStr + ": !throws(" + chk.functionName + "(x)) && !isNaN(" + chk.functionName + "(x))";

    uint32_t numParams = static_cast<uint32_t>(fn.callerParamCount());
    BVContext ctx;
    std::vector<BVNodeId> argNodes;
    for (uint32_t i = 0; i < numParams; ++i) {
        std::string pname = (fn.firstSourceParam() + i < fn.params.size() && !fn.params[fn.firstSourceParam() + i].name.empty())
                                ? fn.params[fn.firstSourceParam() + i].name : ("arg_" + std::to_string(i));
        chk.paramNames.push_back(pname);
        if (floatMode) {
            argNodes.push_back(ctx.make_var("arg_" + std::to_string(i), 64, true));
        } else {
            argNodes.push_back(ctx.make_sext(ctx.make_var("arg_" + std::to_string(i), 32), 32));
        }
    }

    SymbolicHeapTable heapTable(ctx, floatMode);
    heapTable.setPreferStringSeq(strMode);
    BVNodeId retNode = 0, threwNode = 0, exceededNode = 0;
    ProofBounds bounds;
    std::string reason;
    SymbolicHeapState finalHeap;
    bool hitUnroll = false;

    if (!evalIlFunctionSymbolic(ctx, mod, fn, argNodes, true, &retNode, &reason,
                                &heapTable, &finalHeap, &hitUnroll, &threwNode, &exceededNode, &bounds)) {
        outAnalyzed = false;
        chk.status = EquivStatus::Unknown;
        chk.detail = reason;
        return chk;
    }
    outAnalyzed = true;
    if (heapTable.usedBoundedArrayLen() && bounds.maxArrayLength == 0) {
        bounds.maxArrayLength = static_cast<uint32_t>(kMaxBoundedSeqLen);
    }
    if (heapTable.usedBoundedStringLen() && bounds.maxStringLength == 0) {
        bounds.maxStringLength = static_cast<uint32_t>(kMaxBoundedSeqLen);
    }
    if (hitUnroll && bounds.maxLoopIterations == 0) bounds.maxLoopIterations = 8;
    chk.bounds = bounds;
    bool isBounded = hitUnroll || bounds.hasBounds();

    BVNodeId zero1 = ctx.make_const(0, 1);
    BVNodeId tNode = (threwNode != 0) ? threwNode : zero1;
    if (tNode == zero1) {
        chk.status = isBounded ? EquivStatus::Bounded : EquivStatus::Equivalent;
        chk.detail = "proven no-throw across all symbolic inputs in domain";
        return chk;
    }

    BVNodeId badCond = tNode;
    if (exceededNode != 0 && exceededNode != zero1) {
        badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_unary(BVOp::Not, exceededNode, 1), 1);
    }
    BVNodeId seqCond = heapTable.buildBoundedSeqConstraintCond();
    if (seqCond != 0) badCond = ctx.make_binary(BVOp::And, badCond, seqCond, 1);
    if (!floatMode && (dMin > INT32_MIN || dMax < INT32_MAX)) {
        BVNodeId minN = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(dMin)), 64);
        BVNodeId maxN = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(dMax)), 64);
        for (BVNodeId a : argNodes) {
            if (dMin > INT32_MIN) badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sge, a, minN, 1), 1);
            if (dMax < INT32_MAX) badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sle, a, maxN, 1), 1);
        }
    }

    SolveOutcome sat = brass::verify::solve_nonzero(ctx, badCond);
    if (sat.status == SolveOutcome::Unsat) {
        chk.status = isBounded ? EquivStatus::Bounded : EquivStatus::Equivalent;
        chk.detail = "proven no-throw inside documented domain";
        return chk;
    }
    if (sat.status == SolveOutcome::Sat) {
        if (!docMeta.hasExplicitRange) {
            chk.inPreconditionBucket = true;
            chk.status = EquivStatus::Equivalent;
            chk.detail = "explicit guard throw outside unrefined type domain (precondition bucket)";
            return chk;
        }
        size_t totalVars = numParams + heapTable.varNodes().size();
        std::vector<uint64_t> model = sat.model;
        if (model.size() < totalVars) model.resize(totalVars, 0);
        std::vector<std::string> replayNames = chk.paramNames;
        std::vector<int64_t> replayArgs(numParams, 0);
        for (uint32_t i = 0; i < numParams; ++i) {
            replayArgs[i] = floatMode ? static_cast<int64_t>(model[i])
                                      : static_cast<int64_t>(static_cast<int32_t>(model[i]));
        }
        for (size_t i = 0; i < heapTable.varNodes().size(); ++i) {
            replayNames.push_back(heapTable.varPaths()[i]);
            replayArgs.push_back(floatMode ? static_cast<int64_t>(model[numParams + i])
                                           : static_cast<int64_t>(static_cast<int32_t>(model[numParams + i])));
        }
        InterpResult run = runIlFunctionConcrete(mod, fn, replayNames, replayArgs, 50000, floatMode);
        if (run.completed && !run.threw) {
            chk.status = EquivStatus::Unknown;
            chk.detail = "prover/replay mismatch";
            return chk;
        }
        chk.status = EquivStatus::Counterexample;
        chk.paramNames = std::move(replayNames);
        chk.counterexampleArgs = std::move(replayArgs);
        chk.newThrows = true;
        chk.detail = "throws inside documented domain";
        return chk;
    }
    chk.status = EquivStatus::Unknown;
    chk.detail = "solver returned UNKNOWN on domain-safety";
    return chk;
}

AuditPropertyCheck checkRoundTripPair(const il::Module& mod,
                                      const il::Function& encFn,
                                      const il::Function& decFn,
                                      const FunctionDocMeta& encMeta) {
    AuditPropertyCheck chk;
    std::string encName = effectiveAuditFnName(encFn);
    std::string decName = effectiveAuditFnName(decFn);
    chk.functionName = decName + "(" + encName + "(x))";
    chk.propertyKind = "round-trip";
    int32_t dMin = encMeta.domainMin;
    int32_t dMax = encMeta.domainMax;
    std::string domStr = formatDomainRange(dMin, dMax, false, false);
    chk.formalProperty = "forall x in " + domStr + ": " + decName + "(" + encName + "(x)) === x";
    chk.paramNames = {"x"};

    BVContext ctx;
    BVNodeId xNode = ctx.make_sext(ctx.make_var("x", 32), 32);
    SymbolicHeapTable heapTable(ctx, false);
    BVNodeId encRet = 0, decRet = 0, encThrew = 0, decThrew = 0, encExc = 0, decExc = 0;
    ProofBounds encBounds, decBounds;
    std::string reason;
    bool encUnroll = false, decUnroll = false;

    if (!evalIlFunctionSymbolic(ctx, mod, encFn, {xNode}, true, &encRet, &reason,
                                &heapTable, nullptr, &encUnroll, &encThrew, &encExc, &encBounds)) {
        chk.status = EquivStatus::Unknown;
        chk.detail = "encode not symbolically decidable: " + reason;
        return chk;
    }
    if (!evalIlFunctionSymbolic(ctx, mod, decFn, {encRet}, true, &decRet, &reason,
                                &heapTable, nullptr, &decUnroll, &decThrew, &decExc, &decBounds)) {
        chk.status = EquivStatus::Unknown;
        chk.detail = "decode not symbolically decidable: " + reason;
        return chk;
    }

    chk.bounds.maxLoopIterations = std::max(encBounds.maxLoopIterations, decBounds.maxLoopIterations);
    chk.bounds.maxArrayLength = std::max(encBounds.maxArrayLength, decBounds.maxArrayLength);
    chk.bounds.maxStringLength = std::max(encBounds.maxStringLength, decBounds.maxStringLength);
    if ((encUnroll || decUnroll) && chk.bounds.maxLoopIterations == 0) chk.bounds.maxLoopIterations = 8;
    bool isBounded = encUnroll || decUnroll || chk.bounds.hasBounds();

    BVNodeId zero1 = ctx.make_const(0, 1);
    BVNodeId eThrew = (encThrew != 0) ? encThrew : zero1;
    BVNodeId dThrew = (decThrew != 0) ? decThrew : zero1;
    BVNodeId eExc = (encExc != 0) ? encExc : zero1;
    BVNodeId dExc = (decExc != 0) ? decExc : zero1;

    BVNodeId badCond = ctx.make_binary(BVOp::Or, dThrew, ctx.make_binary(BVOp::Ne, decRet, xNode, 1), 1);
    badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_unary(BVOp::Not, eThrew, 1), 1);
    badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_unary(BVOp::Not, ctx.make_binary(BVOp::Or, eExc, dExc, 1), 1), 1);
    BVNodeId seqCond = heapTable.buildBoundedSeqConstraintCond();
    if (seqCond != 0) badCond = ctx.make_binary(BVOp::And, badCond, seqCond, 1);

    if (dMin > INT32_MIN) {
        BVNodeId minN = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(dMin)), 64);
        badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sge, xNode, minN, 1), 1);
    }
    if (dMax < INT32_MAX) {
        BVNodeId maxN = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(dMax)), 64);
        badCond = ctx.make_binary(BVOp::And, badCond, ctx.make_binary(BVOp::Sle, xNode, maxN, 1), 1);
    }

    SolveOutcome sat = brass::verify::solve_nonzero(ctx, badCond);
    if (sat.status == SolveOutcome::Unsat) {
        chk.status = isBounded ? EquivStatus::Bounded : EquivStatus::Equivalent;
        chk.detail = "proven round-trip identity across symbolic domain";
        return chk;
    }
    if (sat.status == SolveOutcome::Sat) {
        int32_t xVal = static_cast<int32_t>(sat.model.empty() ? 0 : sat.model[0]);
        static constexpr int32_t kCands[] = {0, 1, -1, 2, -2, 63, 64, 127, 128, -128, 255, 256, INT32_MAX, INT32_MIN};
        for (int32_t c : kCands) {
            if (c < dMin || c > dMax) continue;
            std::vector<uint64_t> m = sat.model;
            if (m.empty()) m.push_back(0);
            m[0] = static_cast<uint64_t>(static_cast<uint32_t>(c));
            if (ctx.eval_concrete(badCond, m) != 0) {
                xVal = c;
                break;
            }
        }
        InterpResult rEnc = runIlFunctionConcrete(mod, encFn, {"x"}, {static_cast<int64_t>(xVal)});
        if (rEnc.completed && !rEnc.threw) {
            InterpResult rDec = runIlFunctionConcrete(mod, decFn, {"y"}, {rEnc.returnI64});
            if (rDec.completed && !rDec.threw && rDec.returnI64 == static_cast<int64_t>(xVal)) {
                chk.status = EquivStatus::Unknown;
                chk.detail = "prover/replay mismatch";
                return chk;
            }
            if (rDec.completed) {
                chk.status = EquivStatus::Counterexample;
                chk.counterexampleArgs = {static_cast<int64_t>(xVal)};
                chk.expectedValue = static_cast<int64_t>(xVal);
                chk.actualValue = rDec.returnI64;
                chk.newThrows = rDec.threw;
                chk.detail = "round-trip divergence confirmed by concrete replay";
                return chk;
            }
        }
        chk.status = EquivStatus::Counterexample;
        chk.counterexampleArgs = {static_cast<int64_t>(xVal)};
        chk.expectedValue = static_cast<int64_t>(xVal);
        chk.actualValue = static_cast<int64_t>(ctx.eval_concrete(decRet, sat.model));
        chk.detail = "round-trip divergence";
        return chk;
    }
    chk.status = EquivStatus::Unknown;
    chk.detail = "solver returned UNKNOWN on round-trip";
    return chk;
}

void checkBinaryAlgebraicLaws(const il::Module& mod,
                              const il::Function& fn,
                              const FunctionDocMeta& docMeta,
                              PackageAuditReport& report) {
    if (fn.callerParamCount() != 2) return;
    if (docMeta.params.size() >= 2 &&
        docMeta.params[0].type != DocValType::Unknown &&
        docMeta.params[1].type != DocValType::Unknown &&
        docMeta.params[0].type != docMeta.params[1].type) {
        return;
    }
    std::string fnName = effectiveAuditFnName(fn);
    int32_t dMin = docMeta.domainMin;
    int32_t dMax = docMeta.domainMax;
    bool floatMode = (dMin == INT32_MIN && dMax == INT32_MAX) && functionRequiresFloatMode(mod, fn);

    BVContext ctx;
    BVNodeId aNode = floatMode ? ctx.make_var("arg_0", 64, true)
                               : ctx.make_sext(ctx.make_var("arg_0", 32), 32);
    BVNodeId bNode = floatMode ? ctx.make_var("arg_1", 64, true)
                               : ctx.make_sext(ctx.make_var("arg_1", 32), 32);
    SymbolicHeapTable heapTable(ctx, floatMode);
    BVNodeId retAB = 0, retBA = 0, threwAB = 0, threwBA = 0, excAB = 0, excBA = 0;
    ProofBounds boundsAB, boundsBA;
    bool unrollAB = false, unrollBA = false;
    SymbolicHeapState heapAB, heapBA;

    if (!evalIlFunctionSymbolic(ctx, mod, fn, {aNode, bNode}, true, &retAB, nullptr,
                                &heapTable, &heapAB, &unrollAB, &threwAB, &excAB, &boundsAB)) {
        return;
    }
    if (!evalIlFunctionSymbolic(ctx, mod, fn, {bNode, aNode}, true, &retBA, nullptr,
                                &heapTable, &heapBA, &unrollBA, &threwBA, &excBA, &boundsBA)) {
        return;
    }
    if (!heapAB.writes.empty() || !heapBA.writes.empty()) return;

    BVNodeId zero1 = ctx.make_const(0, 1);
    BVNodeId neitherThrew = ctx.make_unary(
        BVOp::Not, ctx.make_binary(BVOp::Or, threwAB ? threwAB : zero1, threwBA ? threwBA : zero1, 1), 1);
    BVNodeId neitherExc = ctx.make_unary(
        BVOp::Not, ctx.make_binary(BVOp::Or, excAB ? excAB : zero1, excBA ? excBA : zero1, 1), 1);
    BVNodeId baseGuard = ctx.make_binary(BVOp::And, neitherThrew, neitherExc, 1);
    BVNodeId seqCond = heapTable.buildBoundedSeqConstraintCond();
    if (seqCond != 0) baseGuard = ctx.make_binary(BVOp::And, baseGuard, seqCond, 1);

    ProofBounds cBounds;
    cBounds.maxLoopIterations = std::max(boundsAB.maxLoopIterations, boundsBA.maxLoopIterations);
    cBounds.maxArrayLength = std::max(boundsAB.maxArrayLength, boundsBA.maxArrayLength);
    cBounds.maxStringLength = std::max(boundsAB.maxStringLength, boundsBA.maxStringLength);
    if ((unrollAB || unrollBA) && cBounds.maxLoopIterations == 0) cBounds.maxLoopIterations = 8;
    bool isBounded = unrollAB || unrollBA || cBounds.hasBounds();

    // 1. Check commutativity: f(a, b) === f(b, a)
    BVNodeId commNe = floatMode
        ? ctx.make_unary(BVOp::Not, ctx.make_binary(
              BVOp::Or,
              ctx.make_binary(BVOp::And, ctx.make_binary(BVOp::FNe, retAB, retAB, 1), ctx.make_binary(BVOp::FNe, retBA, retBA, 1), 1),
              ctx.make_binary(BVOp::Eq, retAB, retBA, 1), 1), 1)
        : ctx.make_binary(BVOp::Ne, retAB, retBA, 1);
    SolveOutcome commSat = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::And, baseGuard, commNe, 1));
    if (commSat.status == SolveOutcome::Unsat) {
        AuditPropertyCheck chk;
        chk.functionName = fnName;
        chk.propertyKind = "commutativity";
        chk.formalProperty = "forall a, b: " + fnName + "(a, b) === " + fnName + "(b, a)";
        chk.status = isBounded ? EquivStatus::Bounded : EquivStatus::Equivalent;
        chk.bounds = cBounds;
        chk.detail = "proven commutative";
        recordVerdictCounts(report, chk);
        report.checks.push_back(std::move(chk));
        return;
    }

    // 2. Check antisymmetry: f(a, b) === -f(b, a)
    BVNodeId negBA = floatMode
        ? ctx.make_unary(BVOp::FNeg, retBA, 64)
        : ctx.make_binary(BVOp::Sub, ctx.make_const(0, 64), retBA, 64);
    BVNodeId antiNe = floatMode
        ? ctx.make_unary(BVOp::Not, ctx.make_binary(
              BVOp::Or,
              ctx.make_binary(BVOp::And, ctx.make_binary(BVOp::FNe, retAB, retAB, 1), ctx.make_binary(BVOp::FNe, negBA, negBA, 1), 1),
              ctx.make_binary(BVOp::FEq, retAB, negBA, 1), 1), 1)
        : ctx.make_binary(BVOp::Ne, retAB, negBA, 1);
    BVNodeId antiCond = ctx.make_binary(BVOp::And, baseGuard, antiNe, 1);
    SolveOutcome antiSat = brass::verify::solve_nonzero(ctx, antiCond);
    if (antiSat.status == SolveOutcome::Unsat) {
        AuditPropertyCheck chk;
        chk.functionName = fnName;
        chk.propertyKind = "antisymmetry";
        chk.formalProperty = "forall a, b: " + fnName + "(a, b) === -" + fnName + "(b, a)";
        chk.status = isBounded ? EquivStatus::Bounded : EquivStatus::Equivalent;
        chk.bounds = cBounds;
        chk.detail = "proven antisymmetric";
        recordVerdictCounts(report, chk);
        report.checks.push_back(std::move(chk));
        return;
    }

    if ((docMeta.documentedAntisymmetric || docMeta.documentedCompare) && antiSat.status == SolveOutcome::Sat) {
        int64_t aVal = !antiSat.model.empty() ? static_cast<int64_t>(static_cast<int32_t>(antiSat.model[0])) : 0;
        int64_t bVal = (antiSat.model.size() >= 2) ? static_cast<int64_t>(static_cast<int32_t>(antiSat.model[1])) : 0;
        InterpResult rAB = runIlFunctionConcrete(mod, fn, {"a", "b"}, {aVal, bVal}, 50000, floatMode);
        InterpResult rBA = runIlFunctionConcrete(mod, fn, {"a", "b"}, {bVal, aVal}, 50000, floatMode);
        if (rAB.completed && rBA.completed && !rAB.threw && !rBA.threw && rAB.returnI64 != -rBA.returnI64) {
            AuditPropertyCheck chk;
            chk.functionName = fnName;
            chk.propertyKind = "compare-antisymmetry";
            chk.formalProperty = "forall a, b: " + fnName + "(a, b) === -" + fnName + "(b, a)";
            chk.status = EquivStatus::Counterexample;
            chk.paramNames = {"a", "b"};
            chk.counterexampleArgs = {aVal, bVal};
            chk.expectedValue = -rBA.returnI64;
            chk.actualValue = rAB.returnI64;
            chk.detail = "antisymmetry violated and confirmed by concrete replay";
            recordVerdictCounts(report, chk);
            report.checks.push_back(std::move(chk));
        }
    }
}

}  // namespace

PackageAuditReport auditPackage(const std::string& targetPath,
                                const std::string& fnFilter,
                                const std::vector<modules::ModuleRoot>& moduleRoots,
                                const std::string& importMapPath) {
    PackageAuditReport report;
    std::vector<std::string> entryFiles;
    std::map<std::string, FunctionDocMeta> docMeta;
    std::string err;
    if (!discoverPackageAuditTarget(targetPath, report.packageName, report.packageVersion, entryFiles, docMeta, err)) {
        report.unknownCount = 1;
        report.unknownReasons[err] = 1;
        return report;
    }
    report.entryPath = entryFiles.front();

    std::vector<il::Module> loadedModules;
    for (const std::string& ef : entryFiles) {
        il::Module m;
        std::string loadErr;
        if (lowerEntryFileForAudit(ef, moduleRoots, importMapPath, m, loadErr)) {
            loadedModules.push_back(std::move(m));
            if (loadedModules.size() >= 8) break;
        } else if (loadedModules.empty() && ef == entryFiles.back()) {
            report.unknownCount = 1;
            report.unknownReasons["module load/lower failed"] = 1;
            return report;
        }
    }

    std::set<std::string> seenFnNames;
    for (const il::Module& mod : loadedModules) {
        std::vector<const il::Function*> pubFns;
        for (const auto& fn : mod.functions) {
            if (!isCandidatePublicFunction(fn)) continue;
            std::string name = effectiveAuditFnName(fn);
            if (!fnFilter.empty() && name != fnFilter && shortMethodName(name) != fnFilter) continue;
            if (!seenFnNames.insert(name).second) continue;
            pubFns.push_back(&fn);
        }
        report.totalPublicFunctions += static_cast<uint32_t>(pubFns.size());

        for (const il::Function* fn : pubFns) {
            FunctionDocMeta meta = lookupFunctionDocMeta(docMeta, *fn);
            bool analyzed = false;
            AuditPropertyCheck domChk = checkFunctionDomainSafety(mod, *fn, meta, analyzed);
            if (analyzed) report.analyzedFunctions++;
            recordVerdictCounts(report, domChk);
            report.checks.push_back(std::move(domChk));

            if (analyzed) {
                checkBinaryAlgebraicLaws(mod, *fn, meta, report);
            }
        }

        // Round-trip checks on morphologically paired public functions in the same module.
        for (size_t i = 0; i < pubFns.size(); ++i) {
            for (size_t j = 0; j < pubFns.size(); ++j) {
                if (i == j) continue;
                const il::Function& enc = *pubFns[i];
                const il::Function& dec = *pubFns[j];
                if (enc.callerParamCount() == 0 || dec.callerParamCount() == 0) continue;
                if (!areMorphologicalInverseNames(effectiveAuditFnName(enc), effectiveAuditFnName(dec))) continue;
                FunctionDocMeta encMeta = lookupFunctionDocMeta(docMeta, enc);
                FunctionDocMeta decMeta = lookupFunctionDocMeta(docMeta, dec);
                if (!areTypesRoundTripCompatible(encMeta, decMeta)) continue;

                AuditPropertyCheck rtChk = checkRoundTripPair(mod, enc, dec, encMeta);
                recordVerdictCounts(report, rtChk);
                report.checks.push_back(std::move(rtChk));
            }
        }

        // Sibling agreement checks on paired exports with the same documented domain.
        for (size_t i = 0; i < pubFns.size(); ++i) {
            for (size_t j = i + 1; j < pubFns.size(); ++j) {
                const il::Function& fA = *pubFns[i];
                const il::Function& fB = *pubFns[j];
                if (fA.callerParamCount() != fB.callerParamCount() || fA.callerParamCount() == 0) continue;
                std::string nA = effectiveAuditFnName(fA);
                std::string nB = effectiveAuditFnName(fB);
                if (!areSiblingAgreementNames(nA, nB)) continue;
                FunctionDocMeta mA = lookupFunctionDocMeta(docMeta, fA);
                FunctionDocMeta mB = lookupFunctionDocMeta(docMeta, fB);
                if (mA.domainMin != mB.domainMin || mA.domainMax != mB.domainMax) continue;

                FunctionEquivResult eq = compareIlFunctionPair(mod, fA, mod, fB, true, mA.domainMin, mA.domainMax);
                AuditPropertyCheck sibChk;
                sibChk.functionName = nA + " == " + nB;
                sibChk.propertyKind = "sibling-agreement";
                sibChk.formalProperty = "forall x in " + formatDomainRange(mA.domainMin, mA.domainMax, false, false) +
                                        ": " + nA + "(x) === " + nB + "(x)";
                sibChk.status = eq.status;
                sibChk.bounds = eq.bounds;
                sibChk.paramNames = eq.paramNames;
                sibChk.counterexampleArgs = eq.counterexampleArgs;
                sibChk.expectedValue = eq.oldReturnValue;
                sibChk.actualValue = eq.newReturnValue;
                sibChk.oldThrows = eq.oldThrows;
                sibChk.newThrows = eq.newThrows;
                sibChk.detail = eq.detail;
                recordVerdictCounts(report, sibChk);
                report.checks.push_back(std::move(sibChk));
            }
        }
    }

    return report;
}

PackageAuditReport auditPackageDiff(const std::string& oldTargetPath,
                                    const std::string& newTargetPath,
                                    const std::string& fnFilter,
                                    const std::vector<modules::ModuleRoot>& moduleRoots,
                                    const std::string& importMapPath) {
    PackageAuditReport report;
    report.isDiffMode = true;
    std::string newPkgName, newPkgVer, err;
    std::vector<std::string> oldEntries, newEntries;
    std::map<std::string, FunctionDocMeta> oldDoc, newDoc;
    if (!discoverPackageAuditTarget(oldTargetPath, report.packageName, report.packageVersion, oldEntries, oldDoc, err) ||
        !discoverPackageAuditTarget(newTargetPath, newPkgName, newPkgVer, newEntries, newDoc, err)) {
        report.unknownCount = 1;
        report.unknownReasons[err] = 1;
        return report;
    }
    report.entryPath = oldEntries.front();
    report.secondEntryPath = newPkgName + "@" + newPkgVer;

    il::Module oldMod, newMod;
    if (!lowerEntryFileForAudit(oldEntries.front(), moduleRoots, importMapPath, oldMod, err) ||
        !lowerEntryFileForAudit(newEntries.front(), moduleRoots, importMapPath, newMod, err)) {
        report.unknownCount = 1;
        report.unknownReasons["diff module load/lower failed"] = 1;
        return report;
    }

    for (const auto& oldFn : oldMod.functions) {
        if (!isCandidatePublicFunction(oldFn)) continue;
        std::string name = effectiveAuditFnName(oldFn);
        if (!fnFilter.empty() && name != fnFilter && shortMethodName(name) != fnFilter) continue;
        const il::Function* newFn = nullptr;
        for (const auto& cand : newMod.functions) {
            if (!isCandidatePublicFunction(cand)) continue;
            if (effectiveAuditFnName(cand) == name || shortMethodName(effectiveAuditFnName(cand)) == shortMethodName(name)) {
                newFn = &cand;
                break;
            }
        }
        if (!newFn) continue;
        report.totalPublicFunctions++;

        FunctionDocMeta meta = lookupFunctionDocMeta(newDoc, *newFn);
        FunctionEquivResult eqRes = compareIlFunctionPair(oldMod, oldFn, newMod, *newFn, true, meta.domainMin, meta.domainMax);
        if (eqRes.status != EquivStatus::Unknown) {
            report.analyzedFunctions++;
        }

        AuditPropertyCheck chk;
        chk.functionName = name;
        chk.propertyKind = "diff-equiv";
        chk.formalProperty = "forall x in " + formatDomainRange(meta.domainMin, meta.domainMax, false, false) +
                             ": " + name + "_old(x) === " + name + "_new(x)";
        chk.status = eqRes.status;
        chk.bounds = eqRes.bounds;
        chk.paramNames = eqRes.paramNames;
        chk.counterexampleArgs = eqRes.counterexampleArgs;
        chk.expectedValue = eqRes.oldReturnValue;
        chk.actualValue = eqRes.newReturnValue;
        chk.oldThrows = eqRes.oldThrows;
        chk.newThrows = eqRes.newThrows;
        chk.detail = eqRes.detail;
        recordVerdictCounts(report, chk);
        report.checks.push_back(std::move(chk));
    }

    return report;
}

int runAuditCli(int argc, char** argv, std::ostream& out, std::ostream& err) {
    bool diffMode = false;
    bool jsonOutput = false;
    std::string fnFilter;
    std::vector<std::string> positional;
    std::vector<modules::ModuleRoot> moduleRoots;
    std::string importMapPath;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--diff") {
            diffMode = true;
        } else if (arg == "--json") {
            jsonOutput = true;
        } else if (arg == "--fn" && i + 1 < argc) {
            fnFilter = argv[++i];
        } else if (arg.rfind("--fn=", 0) == 0) {
            fnFilter = arg.substr(5);
        } else if (arg == "--module-root" && i + 1 < argc) {
            std::string val = argv[++i];
            auto eq = val.find('=');
            if (eq != std::string::npos) moduleRoots.push_back({val.substr(0, eq), val.substr(eq + 1)});
        } else if (arg == "--import-map" && i + 1 < argc) {
            importMapPath = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            positional.push_back(arg);
        } else {
            err << "error: unknown audit option: " << arg << "\n";
            return 1;
        }
    }

    if (diffMode) {
        if (positional.size() < 2) {
            err << "usage: bronze audit --diff <old_dir_or_file> <new_dir_or_file> [--json] [--fn <name>]\n";
            return 1;
        }
        PackageAuditReport rep = auditPackageDiff(positional[0], positional[1], fnFilter, moduleRoots, importMapPath);
        out << (jsonOutput ? rep.renderJson() : rep.renderText());
        return (rep.counterexampleCount > 0) ? 1 : 0;
    }

    if (positional.empty()) {
        err << "usage: bronze audit <pkg_dir_or_file> [--json] [--fn <name>]\n";
        return 1;
    }
    PackageAuditReport rep = auditPackage(positional[0], fnFilter, moduleRoots, importMapPath);
    out << (jsonOutput ? rep.renderJson() : rep.renderText());
    return (rep.counterexampleCount > 0) ? 1 : 0;
}

}  // namespace bronze::check
