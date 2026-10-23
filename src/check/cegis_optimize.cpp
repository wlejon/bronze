#include "check/cegis_optimize.h"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include "check/cegis_superopt.h"
#include "check/prove_equiv.h"
#include "check/solve_fn.h"

namespace bronze::check {
namespace {

std::string replaceAll(std::string str, std::string_view from, std::string_view to) {
    if (from.empty()) return str;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}

std::vector<std::string> extractParamNamesFromNormalized(std::string_view normSrc) {
    std::vector<std::string> params;
    size_t openParen = normSrc.find('(');
    if (openParen == std::string_view::npos) return params;
    size_t closeParen = normSrc.find(')', openParen + 1);
    if (closeParen == std::string_view::npos) return params;

    std::string_view inner = normSrc.substr(openParen + 1, closeParen - openParen - 1);
    size_t start = 0;
    while (start < inner.size()) {
        size_t comma = inner.find(',', start);
        std::string_view tok = (comma == std::string_view::npos)
                                   ? inner.substr(start)
                                   : inner.substr(start, comma - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.remove_prefix(1);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.remove_suffix(1);
        size_t eq = tok.find('=');
        if (eq != std::string_view::npos) {
            tok = tok.substr(0, eq);
            while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.remove_suffix(1);
        }
        if (!tok.empty()) params.emplace_back(tok);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return params;
}

// Inserts a concrete boundary guard `if (p0 === v0 && ...) return expected;`
// at the entry of `normCandidate`.
std::string injectBoundaryGuard(std::string_view normCandidate,
                                const std::vector<std::string>& paramNames,
                                const CounterexampleInfo& cex) {
    size_t openBrace = normCandidate.find('{');
    if (openBrace == std::string_view::npos || cex.args.empty()) {
        return std::string(normCandidate);
    }
    std::ostringstream guard;
    guard << " if (";
    for (size_t i = 0; i < cex.args.size(); ++i) {
        if (i > 0) guard << " && ";
        std::string pname = (i < paramNames.size()) ? paramNames[i] : ("arg_" + std::to_string(i));
        guard << pname << " === " << cex.args[i];
    }
    guard << ") return " << cex.expectedOutput << ";";

    std::string patched(normCandidate.substr(0, openBrace + 1));
    patched += guard.str();
    patched += normCandidate.substr(openBrace + 1);
    return patched;
}

// Generates built-in strength-reduction and closed-form loop candidates for `normRef`.
std::vector<std::string> generateBuiltinCandidates(const std::string& normRef,
                                                   const std::string& fnName,
                                                   const std::vector<std::string>& params) {
    std::vector<std::string> out;

    // 1. Algebraic strength reductions on expressions inside normRef
    struct RewriteRule {
        const char* pattern;
        const char* replacement;
    };
    static constexpr RewriteRule kRules[] = {
        {" * 2", " << 1"},
        {" * 4", " << 2"},
        {" * 8", " << 3"},
        {" * 16", " << 4"},
        {" / 2) | 0", " >> 1)"},
        {" / 4) | 0", " >> 2)"},
        {" / 8) | 0", " >> 3)"},
        {" % 2", " & 1"},
        {" % 4", " & 3"},
        {" % 8", " & 7"},
        {" % 16", " & 15"},
    };

    std::string combined = normRef;
    for (const auto& rule : kRules) {
        if (normRef.find(rule.pattern) != std::string::npos) {
            out.push_back(replaceAll(normRef, rule.pattern, rule.replacement));
            combined = replaceAll(combined, rule.pattern, rule.replacement);
        }
    }
    if (combined != normRef) {
        out.push_back(combined);
    }

    if (!params.empty()) {
        const std::string& p0 = params[0];
        // Math.pow(p0, 2) -> (p0 * p0)
        std::string pow2 = "Math.pow(" + p0 + ", 2)";
        if (normRef.find(pow2) != std::string::npos) {
            out.push_back(replaceAll(normRef, pow2, "(" + p0 + " * " + p0 + ")"));
        }
        std::string pow3 = "Math.pow(" + p0 + ", 3)";
        if (normRef.find(pow3) != std::string::npos) {
            out.push_back(replaceAll(normRef, pow3, "(" + p0 + " * " + p0 + " * " + p0 + ")"));
        }
        // Math.abs(p0) -> branchless bit-twiddling abs
        std::string absCall = "Math.abs(" + p0 + ")";
        if (normRef.find(absCall) != std::string::npos) {
            std::string babs = "((" + p0 + " ^ (" + p0 + " >> 31)) - (" + p0 + " >> 31))";
            out.push_back(replaceAll(normRef, absCall, babs));
        }

        // Closed-form summation candidates when source contains a `for` or `while` loop
        if (normRef.find("for") != std::string::npos || normRef.find("while") != std::string::npos) {
            // sum 0 .. n-1: n <= 0 ? 0 : ((n * (n - 1)) / 2) | 0
            out.push_back("export function " + fnName + "(" + p0 + ") { return " + p0 +
                          " <= 0 ? 0 : (((" + p0 + " * (" + p0 + " - 1)) / 2) | 0); }");
            // sum 0 .. n: n <= 0 ? 0 : ((n * (n + 1)) / 2) | 0
            out.push_back("export function " + fnName + "(" + p0 + ") { return " + p0 +
                          " <= 0 ? 0 : (((" + p0 + " * (" + p0 + " + 1)) / 2) | 0); }");
        }
    }

    return out;
}

}  // namespace

OptimizeResult optimizeJsFunction(std::string_view refSource, const OptimizeOptions& opts) {
    OptimizeResult result;
    result.domainMin = opts.domainMin;
    result.domainMax = opts.domainMax;

    std::string refFnName;
    std::string defaultName = opts.fnName.empty() ? "__opt_target" : opts.fnName;
    result.referenceSource = normalizeJsFunctionSource(refSource, &refFnName, defaultName);
    if (refFnName.empty()) refFnName = defaultName;

    std::vector<std::string> paramNames = extractParamNamesFromNormalized(result.referenceSource);

    // Build initial candidate queue: user-supplied candidates first, then built-in synthesis templates.
    std::vector<std::string> candidateQueue;
    for (const auto& c : opts.candidates) {
        candidateQueue.push_back(normalizeJsFunctionSource(c, nullptr, refFnName));
    }
    auto builtins = generateBuiltinCandidates(result.referenceSource, refFnName, paramNames);
    for (auto& b : builtins) {
        candidateQueue.push_back(std::move(b));
    }

    // If no candidates exist and proposeCandidate is provided, ask for initial proposal (round 0).
    if (candidateQueue.empty() && opts.proposeCandidate) {
        CounterexampleInfo initCex;
        std::string first = opts.proposeCandidate(result.referenceSource, initCex);
        if (!first.empty()) {
            candidateQueue.push_back(normalizeJsFunctionSource(first, nullptr, refFnName));
        }
    }

    uint32_t round = 0;
    size_t qIdx = 0;
    while (qIdx < candidateQueue.size() && round < opts.maxRounds) {
        std::string cand = candidateQueue[qIdx++];
        if (cand.empty() || cand == result.referenceSource) continue;

        ++round;
        std::string errMsg;
        EquivReport eq = proveSourceEquivalent(
            result.referenceSource, cand, refFnName, opts.domainMin, opts.domainMax, &errMsg);

        if (eq.status == EquivStatus::Equivalent) {
            result.verified = true;
            result.optimized = true;
            result.optimizedSource = cand;
            result.roundsUsed = round;
            result.summary = "CEGIS proved candidate equivalent in " + std::to_string(round) +
                             " round(s) across [" + std::to_string(opts.domainMin) + ", " +
                             std::to_string(opts.domainMax) + "]";
            return result;
        }

        if (eq.status == EquivStatus::Counterexample) {
            CounterexampleInfo cex;
            cex.round = round;
            cex.candidateSource = cand;
            cex.args = eq.counterexampleArgs;
            cex.expectedOutput = eq.expectedOutput;
            cex.actualOutput = eq.actualOutput;
            result.counterexampleHistory.push_back(cex);

            if (opts.proposeCandidate && round < opts.maxRounds) {
                std::string nextCand = opts.proposeCandidate(cand, cex);
                if (!nextCand.empty()) {
                    candidateQueue.insert(
                        candidateQueue.begin() + static_cast<std::ptrdiff_t>(qIdx),
                        normalizeJsFunctionSource(nextCand, nullptr, refFnName));
                    continue;
                }
            }
            if (opts.autoPatchCounterexamples && round < opts.maxRounds &&
                result.counterexampleHistory.size() <= 4) {
                std::string patched = injectBoundaryGuard(cand, paramNames, cex);
                if (patched != cand) {
                    candidateQueue.insert(
                        candidateQueue.begin() + static_cast<std::ptrdiff_t>(qIdx),
                        std::move(patched));
                }
            }
        }
    }

    // If no template candidate matched, invoke the enumerative CEGIS superoptimizer (#142).
    if (opts.candidates.empty() && !opts.proposeCandidate) {
        CegisSuperoptOptions sopts;
        sopts.domainMin = opts.domainMin;
        sopts.domainMax = opts.domainMax;
        auto sres = superoptimizeJsFunction(result.referenceSource, sopts);
        if (sres.verified && sres.optimized) {
            result.verified = true;
            result.optimized = true;
            result.optimizedSource = sres.optimizedSource;
            result.roundsUsed = round + sres.stats.smtQueries;
            for (const auto& c : sres.counterexamples) {
                result.counterexampleHistory.push_back(c);
            }
            result.summary = sres.summary;
            return result;
        }
    }

    // Fallback: verify the reference function against itself so `result.verified` reflects
    // whether the reference function is symbolically decidable and verified on the domain.
    std::string selfErr;
    EquivReport selfEq = proveSourceEquivalent(
        result.referenceSource, result.referenceSource, refFnName,
        opts.domainMin, opts.domainMax, &selfErr);
    result.verified = (selfEq.status == EquivStatus::Equivalent);
    result.optimized = false;
    result.optimizedSource = result.referenceSource;
    result.roundsUsed = round;
    result.summary = result.verified
                         ? "reference function verified; no distinct candidate proven equivalent"
                         : ("reference function could not be symbolically verified: " + selfEq.detail);
    return result;
}

}  // namespace bronze::check
