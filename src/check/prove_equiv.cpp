#include "check/prove_equiv.h"

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

bool functionMatchesName(const il::Function& fn, std::string_view query) {
    if (fn.name == query) return true;
    auto pos = fn.name.rfind("::");
    if (pos != std::string::npos && std::string_view(fn.name).substr(pos + 2) == query) {
        return true;
    }
    return false;
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
        if (f.status == EquivStatus::Counterexample) {
            os << " (";
            for (size_t i = 0; i < f.counterexampleArgs.size(); ++i) {
                if (i > 0) os << ", ";
                std::string pname = (i < f.paramNames.size()) ? f.paramNames[i] : ("arg_" + std::to_string(i));
                os << pname << " = " << f.counterexampleArgs[i];
            }
            os << ") -> old = " << f.oldReturnValue << ", new = " << f.newReturnValue;
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
        if (f.status == EquivStatus::Counterexample) {
            os << ",\"counterexample\":{\"args\":[";
            for (size_t j = 0; j < f.counterexampleArgs.size(); ++j) {
                if (j > 0) os << ",";
                os << f.counterexampleArgs[j];
            }
            os << "],\"oldReturn\":" << f.oldReturnValue
               << ",\"newReturn\":" << f.newReturnValue << "}";
        }
        os << "}";
    }
    os << "],\"allEquivalent\":" << (allEquivalent() ? "true" : "false") << "}\n";
    return os.str();
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
        const il::Function* matchedNew = nullptr;
        for (const auto& newFn : newMod.functions) {
            if (newFn.isExternal || newFn.isEntryPoint || newFn.name == "main") continue;
            if (functionMatchesName(newFn, oldFn.name) || functionMatchesName(oldFn, newFn.name)) {
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
    const il::Function* oldFn = nullptr;
    const il::Function* newFn = nullptr;
    for (const auto& f : oldMod.functions) {
        if (!f.isExternal && !f.isEntryPoint && f.name != "main" &&
            (fnFilter.empty() || functionMatchesName(f, fnFilter))) {
            oldFn = &f;
            break;
        }
    }
    for (const auto& f : newMod.functions) {
        if (!f.isExternal && !f.isEntryPoint && f.name != "main" &&
            (fnFilter.empty() || functionMatchesName(f, fnFilter))) {
            newFn = &f;
            break;
        }
    }
    if (!oldFn || !newFn) {
        rep.status = EquivStatus::Unknown;
        rep.detail = "target function not found";
        return rep;
    }
    FunctionEquivResult fnRes = compareIlFunctionPair(oldMod, *oldFn, newMod, *newFn, true, domainMin, domainMax);
    rep.status = fnRes.status;
    rep.fnName = fnRes.name;
    rep.detail = fnRes.detail;
    if (!fnRes.counterexampleArgs.empty()) {
        rep.counterexampleInput = fnRes.counterexampleArgs[0];
        rep.counterexampleArgs = fnRes.counterexampleArgs;
    }
    rep.expectedOutput = fnRes.oldReturnValue;
    rep.actualOutput = fnRes.newReturnValue;
    rep.functions.push_back(std::move(fnRes));
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
    std::string reason;
    if (!evalIlFunctionSymbolic(ctx, mod, *encFn, {xNode}, true, &encRet, &reason)) {
        rep.status = EquivStatus::Unknown;
        rep.detail = "encode function not symbolically decidable: " + reason;
        return rep;
    }
    if (!evalIlFunctionSymbolic(ctx, mod, *decFn, {encRet}, true, &decRet, &reason)) {
        rep.status = EquivStatus::Unknown;
        rep.detail = "decode function not symbolically decidable: " + reason;
        return rep;
    }
    BVNodeId badCond = ctx.make_binary(BVOp::Ne, decRet, xNode);
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
