#include "check/solve_fn.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/prove_equiv.h"

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

std::string_view trimView(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

bool functionMatchesFilter(const il::Function& fn, std::string_view query) {
    if (query.empty()) return true;
    if (fn.name == query) return true;
    auto pos = fn.name.rfind("::");
    if (pos != std::string::npos && std::string_view(fn.name).substr(pos + 2) == query) {
        return true;
    }
    return false;
}

const il::Function* findTargetFunction(const il::Module& mod, std::string_view filter) {
    for (const auto& f : mod.functions) {
        if (f.isExternal || f.isEntryPoint || f.name == "main") continue;
        if (functionMatchesFilter(f, filter)) return &f;
    }
    for (const auto& f : mod.functions) {
        if (!f.isExternal && !f.isEntryPoint && f.name != "main") return &f;
    }
    return nullptr;
}

// Find top-level `=>` outside parentheses/brackets/braces/strings.
size_t findTopLevelFatArrow(std::string_view sv) {
    int parenDepth = 0;
    int braceDepth = 0;
    int bracketDepth = 0;
    char quote = 0;
    for (size_t i = 0; i + 1 < sv.size(); ++i) {
        char c = sv[i];
        if (quote != 0) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            continue;
        }
        if (c == '(') ++parenDepth;
        else if (c == ')' && parenDepth > 0) --parenDepth;
        else if (c == '{') ++braceDepth;
        else if (c == '}' && braceDepth > 0) --braceDepth;
        else if (c == '[') ++bracketDepth;
        else if (c == ']' && bracketDepth > 0) --bracketDepth;
        else if (parenDepth == 0 && braceDepth == 0 && bracketDepth == 0 &&
                 c == '=' && sv[i + 1] == '>') {
            return i;
        }
    }
    return std::string_view::npos;
}

struct PreparedSolverContext {
    il::Module mod;
    const il::Function* fn = nullptr;
    uint32_t numParams = 0;
    std::vector<std::string> paramNames;
    BVContext ctx;
    std::vector<BVNodeId> argNodes;
    BVNodeId retNode = 0;
    BVNodeId goalCond = 0;
};

bool prepareSolverContext(std::string_view fnSource,
                          const SolveOptions& opts,
                          PreparedSolverContext& prep,
                          std::string& errOut) {
    std::string detectedName;
    std::string normalized = normalizeJsFunctionSource(fnSource, &detectedName);
    std::string filter = !opts.fnName.empty() ? opts.fnName : detectedName;

    if (!lowerSingleSourceForEquiv(normalized, "solve_target.js", prep.mod, &errOut)) {
        return false;
    }
    prep.fn = findTargetFunction(prep.mod, filter);
    if (!prep.fn) {
        errOut = "no target function found in source";
        return false;
    }

    prep.numParams = static_cast<uint32_t>(prep.fn->callerParamCount());
    uint32_t firstSrc = static_cast<uint32_t>(prep.fn->firstSourceParam());
    prep.paramNames.reserve(prep.numParams);
    prep.argNodes.reserve(prep.numParams);

    for (uint32_t i = 0; i < prep.numParams; ++i) {
        std::string pname;
        if ((firstSrc + i) < prep.fn->params.size()) {
            pname = prep.fn->params[firstSrc + i].name;
        }
        if (pname.empty()) pname = "arg_" + std::to_string(i);
        prep.paramNames.push_back(pname);

        BVNodeId raw32 = prep.ctx.make_var("arg_" + std::to_string(i), 32);
        prep.argNodes.push_back(prep.ctx.make_sext(raw32, 32));
    }

    std::string reason;
    if (!evalIlFunctionSymbolic(prep.ctx, prep.mod, *prep.fn, prep.argNodes,
                                /*strictJs32Bitwise=*/true, &prep.retNode, &reason)) {
        errOut = "function not symbolically decidable: " + reason;
        return false;
    }

    BVNodeId zero64 = prep.ctx.make_const(0, 64);
    if (opts.matchReturnValue) {
        BVNodeId targetNode = prep.ctx.make_const(static_cast<uint64_t>(opts.targetReturn), 64);
        prep.goalCond = prep.ctx.make_binary(BVOp::Eq, prep.retNode, targetNode);
    } else {
        prep.goalCond = prep.ctx.make_binary(BVOp::Ne, prep.retNode, zero64);
    }

    for (uint32_t i = 0; i < prep.numParams; ++i) {
        int32_t lo = opts.domainMin;
        int32_t hi = opts.domainMax;
        auto it = opts.varBounds.find(prep.paramNames[i]);
        if (it != opts.varBounds.end()) {
            lo = std::max(lo, it->second.first);
            hi = std::min(hi, it->second.second);
        }
        if (lo > INT32_MIN) {
            BVNodeId loNode = prep.ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(lo)), 64);
            prep.goalCond = prep.ctx.make_binary(
                BVOp::And, prep.goalCond, prep.ctx.make_binary(BVOp::Sge, prep.argNodes[i], loNode));
        }
        if (hi < INT32_MAX) {
            BVNodeId hiNode = prep.ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(hi)), 64);
            prep.goalCond = prep.ctx.make_binary(
                BVOp::And, prep.goalCond, prep.ctx.make_binary(BVOp::Sle, prep.argNodes[i], hiNode));
        }
    }

    return true;
}

void refineModelIfRequested(const PreparedSolverContext& prep,
                            BVNodeId activeCond,
                            bool minimize,
                            std::vector<uint64_t>& model) {
    if (!minimize || prep.numParams == 0) return;
    static constexpr int32_t kSmallCandidates[] = {
        0, 1, -1, 2, -2, 3, -3, 4, -4, 5, 6, 7, 8, 9, 10, 12, 16, 20, 24, 32, 42, 50, 64, 100
    };
    for (uint32_t p = 0; p < prep.numParams; ++p) {
        uint64_t orig = model[p];
        int32_t origS32 = static_cast<int32_t>(static_cast<uint32_t>(orig));
        for (int32_t c : kSmallCandidates) {
            if (std::abs(static_cast<int64_t>(c)) >= std::abs(static_cast<int64_t>(origS32))) continue;
            model[p] = static_cast<uint64_t>(static_cast<uint32_t>(c));
            if (prep.ctx.eval_concrete(activeCond, model) != 0) {
                orig = model[p];
                origS32 = c;
                break;
            }
            model[p] = orig;
        }
    }
}

}  // namespace

const char* solveStatusTag(SolveStatus status) {
    switch (status) {
        case SolveStatus::Sat: return "SAT";
        case SolveStatus::Unsat: return "UNSAT";
        case SolveStatus::Unknown: return "UNKNOWN";
        case SolveStatus::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::string normalizeJsFunctionSource(std::string_view raw,
                                      std::string* outFnName,
                                      std::string_view defaultName) {
    std::string_view trimmed = trimView(raw);
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed = trimView(trimmed.substr(0, trimmed.size() - 1));
    }
    // Strip enclosing parentheses if the entire function was wrapped like `(function(x){...})`
    if (trimmed.size() >= 2 && trimmed.front() == '(' && trimmed.back() == ')') {
        // Check if matching closing paren is at the very end
        int depth = 0;
        bool wrapsAll = true;
        char quote = 0;
        for (size_t i = 0; i < trimmed.size(); ++i) {
            char c = trimmed[i];
            if (quote != 0) {
                if (c == '\\') { ++i; continue; }
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
            if (c == '(') ++depth;
            else if (c == ')') {
                --depth;
                if (depth == 0 && i + 1 < trimmed.size()) {
                    wrapsAll = false;
                    break;
                }
            }
        }
        if (wrapsAll) {
            trimmed = trimView(trimmed.substr(1, trimmed.size() - 2));
        }
    }

    // Check if it starts with `export function` or `function`
    std::string_view rest = trimmed;
    bool hasExport = false;
    if (rest.rfind("export ", 0) == 0 || rest.rfind("export\t", 0) == 0 || rest.rfind("export\n", 0) == 0) {
        hasExport = true;
        rest = trimView(rest.substr(6));
    }
    if (rest.rfind("function", 0) == 0 && (rest.size() == 8 || !isIdentChar(rest[8]))) {
        std::string_view afterFn = trimView(rest.substr(8));
        if (!afterFn.empty() && afterFn.front() == '(') {
            // Anonymous `function(a, b) { ... }`
            if (outFnName) *outFnName = std::string(defaultName);
            return "export function " + std::string(defaultName) + std::string(afterFn);
        }
        // Named function: extract identifier if outFnName requested
        if (outFnName) {
            size_t len = 0;
            while (len < afterFn.size() && isIdentChar(afterFn[len])) ++len;
            *outFnName = std::string(afterFn.substr(0, len));
        }
        if (hasExport || trimmed.find("export ") != std::string_view::npos) {
            return std::string(trimmed);
        }
        return "export " + std::string(trimmed);
    }

    // Check for top-level arrow function `(a, b) => ...` or `x => ...`
    size_t arrowPos = findTopLevelFatArrow(trimmed);
    if (arrowPos != std::string_view::npos) {
        std::string_view paramsPart = trimView(trimmed.substr(0, arrowPos));
        std::string_view bodyPart = trimView(trimmed.substr(arrowPos + 2));
        std::string paramsList;
        if (paramsPart.size() >= 2 && paramsPart.front() == '(' && paramsPart.back() == ')') {
            paramsList = std::string(paramsPart);
        } else {
            paramsList = "(" + std::string(paramsPart) + ")";
        }
        if (outFnName) *outFnName = std::string(defaultName);
        if (!bodyPart.empty() && bodyPart.front() == '{') {
            return "export function " + std::string(defaultName) + paramsList + " " + std::string(bodyPart);
        }
        return "export function " + std::string(defaultName) + paramsList + " { return (" +
               std::string(bodyPart) + "); }";
    }

    return std::string(raw);
}

SolveResult solveJsFunction(std::string_view fnSource, const SolveOptions& opts) {
    auto t0 = std::chrono::steady_clock::now();
    SolveResult result;

    PreparedSolverContext prep;
    std::string errMsg;
    if (!prepareSolverContext(fnSource, opts, prep, errMsg)) {
        result.status = SolveStatus::Error;
        result.error = std::move(errMsg);
        return result;
    }

    result.paramNames = prep.paramNames;
    SolveOutcome satRes = brass::verify::solve_nonzero(prep.ctx, prep.goalCond);
    auto t1 = std::chrono::steady_clock::now();
    result.solveTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (satRes.status == SolveOutcome::Unsat) {
        result.status = SolveStatus::Unsat;
        return result;
    }
    if (satRes.status != SolveOutcome::Sat) {
        result.status = SolveStatus::Unknown;
        result.error = "SMT solver returned UNKNOWN";
        return result;
    }

    result.status = SolveStatus::Sat;
    std::vector<uint64_t> model = satRes.model;
    if (model.size() < prep.numParams) model.resize(prep.numParams, 0);
    refineModelIfRequested(prep, prep.goalCond, opts.minimizeResult, model);

    result.args.resize(prep.numParams, 0);
    for (uint32_t i = 0; i < prep.numParams; ++i) {
        int32_t s32 = static_cast<int32_t>(static_cast<uint32_t>(model[i]));
        result.args[i] = static_cast<int64_t>(s32);
        result.modelMap[prep.paramNames[i]] = result.args[i];
        model[i] = static_cast<uint64_t>(static_cast<uint32_t>(s32));
    }
    result.returnValue = static_cast<int64_t>(prep.ctx.eval_concrete(prep.retNode, model));
    return result;
}

std::vector<std::vector<int64_t>> solveAllJsFunction(std::string_view fnSource,
                                                     size_t maxSolutions,
                                                     const SolveOptions& opts) {
    std::vector<std::vector<int64_t>> solutions;
    if (maxSolutions == 0) return solutions;

    PreparedSolverContext prep;
    std::string errMsg;
    if (!prepareSolverContext(fnSource, opts, prep, errMsg)) {
        return solutions;
    }

    BVNodeId activeCond = prep.goalCond;
    for (size_t k = 0; k < maxSolutions; ++k) {
        SolveOutcome satRes = brass::verify::solve_nonzero(prep.ctx, activeCond);
        if (satRes.status != SolveOutcome::Sat) break;

        std::vector<uint64_t> model = satRes.model;
        if (model.size() < prep.numParams) model.resize(prep.numParams, 0);
        refineModelIfRequested(prep, activeCond, opts.minimizeResult, model);

        std::vector<int64_t> tuple(prep.numParams, 0);
        BVNodeId diffClause = 0;
        for (uint32_t i = 0; i < prep.numParams; ++i) {
            int32_t s32 = static_cast<int32_t>(static_cast<uint32_t>(model[i]));
            tuple[i] = static_cast<int64_t>(s32);
            BVNodeId valNode = prep.ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(s32)), 64);
            BVNodeId neNode = prep.ctx.make_binary(BVOp::Ne, prep.argNodes[i], valNode);
            diffClause = (i == 0) ? neNode : prep.ctx.make_binary(BVOp::Or, diffClause, neNode);
        }
        solutions.push_back(std::move(tuple));
        if (prep.numParams == 0) break;
        activeCond = prep.ctx.make_binary(BVOp::And, activeCond, diffClause);
    }

    return solutions;
}

bool evalConcreteJsFunction(std::string_view fnSource,
                            const std::vector<int64_t>& args,
                            int64_t* outReturn,
                            std::string* errorOut,
                            std::string_view fnFilter) {
    SolveOptions opts;
    opts.fnName = std::string(fnFilter);
    PreparedSolverContext prep;
    std::string errMsg;
    if (!prepareSolverContext(fnSource, opts, prep, errMsg)) {
        if (errorOut) *errorOut = errMsg;
        return false;
    }
    std::vector<uint64_t> model(prep.numParams, 0);
    for (uint32_t i = 0; i < prep.numParams && i < args.size(); ++i) {
        int32_t s32 = static_cast<int32_t>(args[i]);
        model[i] = static_cast<uint64_t>(static_cast<uint32_t>(s32));
    }
    int64_t retVal = static_cast<int64_t>(prep.ctx.eval_concrete(prep.retNode, model));
    if (outReturn) *outReturn = retVal;
    return true;
}

}  // namespace bronze::check
