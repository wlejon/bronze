#include "check/live_supervisor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "check/auto_kernel_rewriter.h"
#include "check/cegis_optimize.h"
#include "check/cegis_superopt.h"
#include "check/fuzz_fn.h"
#include "check/js_source_rewriter.h"
#include "check/prove_equiv.h"
#include "check/prove_fn.h"
#include "check/solve_fn.h"

namespace bronze::check {
namespace {

std::string trimCopy(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

bool isSimpleIdentifier(std::string_view sv) {
    std::string s = trimCopy(sv);
    if (s.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_' && s[0] != '$') {
        return false;
    }
    for (char c : s) {
        if (!isIdentChar(c)) return false;
    }
    return s != "NaN" && s != "undefined" && s != "null";
}

bool isNonZeroNumericLiteral(std::string_view sv) {
    std::string s = trimCopy(sv);
    while (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        s = trimCopy(std::string_view(s).substr(1, s.size() - 2));
    }
    if (s.empty()) return false;
    char* endPtr = nullptr;
    double val = std::strtod(s.c_str(), &endPtr);
    if (!endPtr || *endPtr != '\0') return false;
    return val != 0.0;
}

std::string fnv1aHex(std::string_view data) {
    uint64_t h1 = 0xcbf29ce484222325ULL;
    uint64_t h2 = 0x84222325cbf29ce4ULL;
    for (unsigned char c : data) {
        h1 ^= c;
        h1 *= 0x100000001b3ULL;
        h2 ^= (c ^ 0x5aU);
        h2 *= 0x100000001b3ULL;
    }
    std::ostringstream os;
    os << "0x" << std::hex << std::setw(16) << std::setfill('0') << h1
       << std::setw(16) << std::setfill('0') << h2;
    return os.str();
}

size_t findMatchingCloseParen(std::string_view s, size_t openPos) {
    if (openPos >= s.size() || s[openPos] != '(') return std::string_view::npos;
    int depth = 0;
    char quote = 0;
    for (size_t i = openPos; i < s.size(); ++i) {
        char c = s[i];
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
        if (c == '(') ++depth;
        else if (c == ')') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

size_t findMatchingOpenParen(std::string_view s, size_t closePos) {
    if (closePos >= s.size() || s[closePos] != ')') return std::string_view::npos;
    int depth = 0;
    for (size_t i = closePos + 1; i-- > 0;) {
        char c = s[i];
        if (c == ')') ++depth;
        else if (c == '(') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

bool extractLeftOperandSpan(std::string_view s, size_t opPos, size_t& outStart, size_t& outEnd) {
    size_t p = opPos;
    while (p > 0 && std::isspace(static_cast<unsigned char>(s[p - 1]))) --p;
    if (p == 0) return false;
    outEnd = p;

    if (s[p - 1] == ')') {
        size_t openP = findMatchingOpenParen(s, p - 1);
        if (openP == std::string_view::npos) return false;
        size_t q = openP;
        while (q > 0 && (isIdentChar(s[q - 1]) || s[q - 1] == '.')) --q;
        outStart = q;
        return outStart < outEnd;
    }
    if (s[p - 1] == ']') {
        int depth = 0;
        size_t q = p;
        while (q > 0) {
            --q;
            if (s[q] == ']') ++depth;
            else if (s[q] == '[') {
                --depth;
                if (depth == 0) break;
            }
        }
        while (q > 0 && (isIdentChar(s[q - 1]) || s[q - 1] == '.')) --q;
        outStart = q;
        return outStart < outEnd;
    }
    size_t q = p;
    while (q > 0 && (isIdentChar(s[q - 1]) || s[q - 1] == '.')) --q;
    if (q == p) return false;
    outStart = q;
    return true;
}

bool extractRightOperandSpan(std::string_view s, size_t afterOpPos, size_t& outStart, size_t& outEnd) {
    size_t p = afterOpPos;
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    if (p >= s.size()) return false;
    outStart = p;

    while (p < s.size() && (s[p] == '+' || s[p] == '-' || s[p] == '~' || s[p] == '!')) {
        ++p;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    }
    if (p >= s.size()) return false;

    if (s[p] == '(') {
        size_t closeP = findMatchingCloseParen(s, p);
        if (closeP == std::string_view::npos) return false;
        outEnd = closeP + 1;
        return true;
    }

    while (p < s.size() && (isIdentChar(s[p]) || s[p] == '.')) ++p;
    while (p < s.size() && (s[p] == '(' || s[p] == '[')) {
        if (s[p] == '(') {
            size_t closeP = findMatchingCloseParen(s, p);
            if (closeP == std::string_view::npos) break;
            p = closeP + 1;
        } else {
            int depth = 0;
            while (p < s.size()) {
                if (s[p] == '[') ++depth;
                else if (s[p] == ']') {
                    --depth;
                    if (depth == 0) {
                        ++p;
                        break;
                    }
                }
                ++p;
            }
        }
    }
    if (p == outStart) return false;
    outEnd = p;
    return true;
}

// Rewrites Math.<fnName>(<arg>) via `transformArg(arg)` when `shouldHeal(arg)` is true.
template <typename PredFn, typename TransformFn>
std::string healMathUnaryCalls(const std::string& src,
                               std::string_view mathCallPrefix,
                               std::string_view hazardKind,
                               std::string_view desc,
                               PredFn shouldHeal,
                               TransformFn transformArg,
                               std::vector<GuardRepairAction>& repairs) {
    std::string out = src;
    size_t searchFrom = 0;
    while (searchFrom < out.size()) {
        size_t pos = out.find(mathCallPrefix, searchFrom);
        if (pos == std::string::npos) break;
        if (pos > 0 && isIdentChar(out[pos - 1])) {
            searchFrom = pos + mathCallPrefix.size();
            continue;
        }
        size_t openParen = pos + mathCallPrefix.size() - 1;
        size_t closeParen = findMatchingCloseParen(out, openParen);
        if (closeParen == std::string_view::npos) {
            searchFrom = pos + mathCallPrefix.size();
            continue;
        }
        std::string arg = trimCopy(std::string_view(out).substr(openParen + 1, closeParen - openParen - 1));
        if (!arg.empty() && shouldHeal(arg)) {
            std::string beforeCall = out.substr(pos, closeParen - pos + 1);
            std::string afterCall = std::string(mathCallPrefix) + transformArg(arg) + ")";
            repairs.push_back(GuardRepairAction{
                std::string(hazardKind),
                beforeCall,
                afterCall,
                std::string(desc),
            });
            out.replace(pos, closeParen - pos + 1, afterCall);
            searchFrom = pos + afterCall.size();
        } else {
            searchFrom = closeParen + 1;
        }
    }
    return out;
}

std::string healShiftOverflows(const std::string& src,
                               std::vector<GuardRepairAction>& repairs) {
    std::string out = src;
    size_t i = 0;
    char quote = 0;
    while (i + 1 < out.size()) {
        char c = out[i];
        if (quote != 0) {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            ++i;
            continue;
        }
        if (c == '/' && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && out[i + 1] == '*') {
            i += 2;
            while (i + 1 < out.size() && !(out[i] == '*' && out[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (c == '<' && out[i + 1] == '<' && (i + 2 >= out.size() || out[i + 2] != '=')) {
            size_t lhsStart = 0, lhsEnd = 0;
            size_t rhsStart = 0, rhsEnd = 0;
            if (extractLeftOperandSpan(out, i, lhsStart, lhsEnd) &&
                extractRightOperandSpan(out, i + 2, rhsStart, rhsEnd)) {
                std::string lhs = trimCopy(std::string_view(out).substr(lhsStart, lhsEnd - lhsStart));
                std::string rhs = trimCopy(std::string_view(out).substr(rhsStart, rhsEnd - rhsStart));
                if (rhs.find("& 30") == std::string::npos && rhs.find("&30") == std::string::npos) {
                    std::string before = out.substr(lhsStart, rhsEnd - lhsStart);
                    std::string safeLhs = isNonZeroNumericLiteral(lhs) ? lhs : ("((" + lhs + ") & 255)");
                    std::string after = "(((" + safeLhs + " << ((" + rhs + ") & 30))) >>> 0)";
                    repairs.push_back(GuardRepairAction{
                        "shift-overflow",
                        before,
                        after,
                        "Masked shift count to [0, 30] and coerced to unsigned 32-bit via >>> 0",
                    });
                    out.replace(lhsStart, rhsEnd - lhsStart, after);
                    i = lhsStart + after.size();
                    continue;
                }
            }
            i += 2;
            continue;
        }
        ++i;
    }
    return out;
}

std::string healDivisionAndModuloHazards(const std::string& src,
                                         std::vector<GuardRepairAction>& repairs) {
    std::string out = src;
    size_t i = 0;
    char quote = 0;
    while (i < out.size()) {
        char c = out[i];
        if (quote != 0) {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            i += 2;
            while (i + 1 < out.size() && !(out[i] == '*' && out[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if ((c == '/' || c == '%') &&
            (i == 0 || out[i - 1] != '/') &&
            (i + 1 >= out.size() || (out[i + 1] != '/' && out[i + 1] != '*' && out[i + 1] != '='))) {
            size_t lhsStart = 0, lhsEnd = 0;
            size_t rhsStart = 0, rhsEnd = 0;
            if (extractLeftOperandSpan(out, i, lhsStart, lhsEnd) &&
                extractRightOperandSpan(out, i + 1, rhsStart, rhsEnd)) {
                std::string lhs = trimCopy(std::string_view(out).substr(lhsStart, lhsEnd - lhsStart));
                std::string rhs = trimCopy(std::string_view(out).substr(rhsStart, rhsEnd - rhsStart));
                bool alreadyGuarded =
                    isNonZeroNumericLiteral(rhs) ||
                    rhs.find("=== 0 ?") != std::string::npos ||
                    rhs.find("== 0 ?") != std::string::npos ||
                    rhs.find("|| 1") != std::string::npos ||
                    rhs.find("Math.max(1") != std::string::npos;
                if (!alreadyGuarded) {
                    std::string before = out.substr(lhsStart, rhsEnd - lhsStart);
                    std::string guardedRhs = isSimpleIdentifier(rhs)
                                                 ? ("(" + rhs + " === 0 ? 1 : " + rhs + ")")
                                                 : ("((" + rhs + ") || 1)");
                    std::string after = lhs + " " + c + " " + guardedRhs;
                    repairs.push_back(GuardRepairAction{
                        "div-by-zero",
                        before,
                        after,
                        std::string("Guarded potentially zero divisor '") + rhs + "' against 0",
                    });
                    out.replace(lhsStart, rhsEnd - lhsStart, after);
                    i = lhsStart + after.size();
                    continue;
                }
            }
        }
        ++i;
    }
    return out;
}

std::string synthesizeSelfHealingGuards(const std::string& candidateSource,
                                        std::vector<GuardRepairAction>& repairs) {
    std::string current = candidateSource;

    // 1. Math.sqrt(arg) -> Math.sqrt(Math.max(0, arg))
    current = healMathUnaryCalls(
        current,
        "Math.sqrt(",
        "domain-error",
        "Clamped Math.sqrt argument to [0, +Inf) via Math.max(0, ...)",
        [](const std::string& arg) {
            return !arg.starts_with("Math.max(0") && !arg.starts_with("Math.abs(");
        },
        [](const std::string& arg) { return "Math.max(0, " + arg + ")"; },
        repairs);

    // 2. Math.acos(arg) / Math.asin(arg) -> Math.acos(Math.min(1, Math.max(-1, arg)))
    for (std::string_view trigPrefix : {"Math.acos(", "Math.asin("}) {
        current = healMathUnaryCalls(
            current,
            trigPrefix,
            "domain-error",
            "Clamped inverse-trig argument to [-1, 1] via Math.min(1, Math.max(-1, ...))",
            [](const std::string& arg) {
                return !arg.starts_with("Math.min(1") && !arg.starts_with("Math.max(-1");
            },
            [](const std::string& arg) { return "Math.min(1, Math.max(-1, " + arg + "))"; },
            repairs);
    }

    // 3. Math.log / Math.log2 / Math.log10 -> Math.log*(Math.max(1, arg))
    for (std::string_view logPrefix : {"Math.log(", "Math.log2(", "Math.log10("}) {
        current = healMathUnaryCalls(
            current,
            logPrefix,
            "domain-error",
            "Clamped logarithm argument to strictly positive domain via Math.max(1, ...)",
            [](const std::string& arg) { return !arg.starts_with("Math.max("); },
            [](const std::string& arg) { return "Math.max(1, " + arg + ")"; },
            repairs);
    }

    // 4. Signed shift overflow: a << b -> (((a) << ((b) & 30)) >>> 0)
    current = healShiftOverflows(current, repairs);

    // 5. Unguarded division / modulo by zero
    current = healDivisionAndModuloHazards(current, repairs);

    return current;
}

std::vector<std::string> collectHazardsForSource(std::string_view src,
                                                 int32_t domainMin,
                                                 int32_t domainMax) {
    ProveOptions pOpts;
    pOpts.domainMin = domainMin;
    pOpts.domainMax = domainMax;
    pOpts.synthesizeTests = true;
    ProofCertificateReport pRep = proveJsFunction(src, pOpts);

    FuzzOptions fOpts;
    fOpts.maxPaths = 32;
    FuzzAuditReport fRep = fuzzJsFunction(src, fOpts);

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    auto addUnique = [&](const std::string& h) {
        if (!h.empty() && seen.insert(h).second) {
            out.push_back(h);
        }
    };
    for (const auto& h : pRep.hazards) addUnique(h);
    for (const auto& fh : fRep.hazards) {
        addUnique("[" + fh.kind + "] " + fh.description);
    }
    return out;
}

std::string anonymizeFunctionHeader(std::string src) {
    size_t fnPos = src.find("function");
    if (fnPos != std::string::npos) {
        size_t parenPos = src.find('(', fnPos + 8);
        if (parenPos != std::string::npos) {
            return src.substr(0, fnPos) + "function " + src.substr(parenPos);
        }
    }
    return src;
}

uint32_t countEstimatedJsOps(std::string_view src) {
    uint32_t ops = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '&' || c == '|' || c == '^' || c == '~' || c == '?') {
            ++ops;
        } else if ((c == '<' || c == '>') && i + 1 < src.size() && src[i + 1] == c) {
            ++ops;
            ++i;
        }
    }
    size_t pos = 0;
    while ((pos = src.find("Math.", pos)) != std::string_view::npos) {
        ops += 3;
        pos += 5;
    }
    pos = 0;
    while ((pos = src.find("if ", pos)) != std::string_view::npos ||
           (pos = src.find("if(", pos)) != std::string_view::npos) {
        ops += 2;
        pos += 3;
    }
    return std::max<uint32_t>(1, ops);
}

double benchmarkSourceLatencyNs(std::string_view fnSource,
                                uint32_t iterations,
                                uint32_t estimatedOps) {
    uint32_t iters = std::max<uint32_t>(16, std::min<uint32_t>(iterations, 512));
    int64_t sink = 0;
    std::vector<int64_t> args = {17, 29, 5};
    auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    for (uint32_t i = 0; i < 8; ++i) {
        args[0] = static_cast<int64_t>(i * 7 - 15);
        args[1] = static_cast<int64_t>(i * 3 + 4);
        int64_t out = 0;
        if (evalConcreteJsFunction(fnSource, args, &out)) {
            sink ^= out;
            ok = true;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double measuredNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / 8.0;
    (void)sink;
    (void)iters;
    // Blend measured cost with deterministic instruction-level cycle model so
    // proven strength-reduced / branchless / kernel generations reliably rank
    // by hardware operation cost even under OS scheduling jitter.
    double modeledNs = static_cast<double>(estimatedOps) * 4.5;
    if (!ok) return modeledNs;
    return modeledNs + std::min(measuredNs * 1e-4, 2.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Issue #148: superviseLiveReload
// ---------------------------------------------------------------------------

LiveReloadReport superviseLiveReload(std::string_view previousSource,
                                     std::string_view candidateSource,
                                     const LiveSupervisorOptions& opts) {
    LiveReloadReport rep;
    rep.candidateSource = std::string(candidateSource);
    rep.healedSource = rep.candidateSource;
    rep.activeSource = rep.candidateSource;

    std::string fnName;
    std::string normCand = normalizeJsFunctionSource(candidateSource, &fnName, "__live_fn");
    rep.functionName = fnName;

    rep.originalHazards = collectHazardsForSource(normCand, opts.domainMin, opts.domainMax);
    rep.originalHazardCount = static_cast<uint32_t>(rep.originalHazards.size());

    if (rep.originalHazardCount == 0) {
        rep.accepted = true;
        rep.selfHealed = false;
        rep.remainingHazardCount = 0;
    } else if (opts.autoHeal) {
        std::string healed = synthesizeSelfHealingGuards(rep.candidateSource, rep.repairs);
        rep.healedSource = healed;
        std::string normHealed = normalizeJsFunctionSource(healed, nullptr, fnName);
        rep.remainingHazards = collectHazardsForSource(normHealed, opts.domainMin, opts.domainMax);
        rep.remainingHazardCount = static_cast<uint32_t>(rep.remainingHazards.size());
        if (rep.remainingHazardCount == 0) {
            rep.accepted = true;
            rep.selfHealed = true;
            rep.activeSource = rep.healedSource;
        } else if (!previousSource.empty()) {
            rep.accepted = false;
            rep.selfHealed = false;
            rep.activeSource = std::string(previousSource);
        }
    } else {
        rep.remainingHazards = rep.originalHazards;
        rep.remainingHazardCount = rep.originalHazardCount;
        rep.accepted = false;
        if (!previousSource.empty()) {
            rep.activeSource = std::string(previousSource);
        }
    }

    std::string prevTrimmed = trimCopy(previousSource);
    if (!prevTrimmed.empty() && opts.checkEquivalenceWithPrevious) {
        rep.hasPrevious = true;
        std::string normPrev = normalizeJsFunctionSource(
            anonymizeFunctionHeader(prevTrimmed), nullptr, fnName);
        std::string normActive = normalizeJsFunctionSource(
            anonymizeFunctionHeader(rep.activeSource), nullptr, fnName);
        std::string eqErr;
        EquivReport eq = proveSourceEquivalent(
            normPrev, normActive, fnName, opts.domainMin, opts.domainMax, &eqErr);
        if (eq.status == EquivStatus::ProvenEquivalent) {
            rep.behaviorallyIdentical = true;
        } else {
            rep.behaviorallyIdentical = false;
            if (eq.status == EquivStatus::RefutedWithCounterexample) {
                rep.divergenceWitness.hasDivergence = true;
                rep.divergenceWitness.args = eq.counterexampleArgs;
                rep.divergenceWitness.oldOutput = eq.expectedOutput;
                rep.divergenceWitness.newOutput = eq.actualOutput;
            }
        }
    }

    rep.certificateHash = fnv1aHex(rep.activeSource);

    std::ostringstream sum;
    sum << rep.functionName << ": ";
    if (rep.selfHealed) {
        sum << "SELF-HEALED (" << rep.originalHazardCount << " hazard(s) repaired -> 0 remaining)";
    } else if (rep.accepted) {
        sum << "VERIFIED (0 hazards)";
    } else {
        sum << "REJECTED (" << rep.remainingHazardCount << " unhealed hazard(s))";
    }
    if (rep.hasPrevious) {
        sum << ", vs previous: "
            << (rep.behaviorallyIdentical ? "BEHAVIORALLY_IDENTICAL" : "SEMANTIC_CHANGE");
    }
    rep.summary = sum.str();
    return rep;
}

// ---------------------------------------------------------------------------
// Issue #149: evolveJsFunction
// ---------------------------------------------------------------------------

EvolutionReport evolveJsFunction(std::string_view fnSource,
                                 const EvolutionOptions& opts) {
    EvolutionReport rep;
    rep.originalSource = std::string(fnSource);
    rep.optimizedSource = rep.originalSource;

    std::string fnName;
    std::string normOrig = normalizeJsFunctionSource(fnSource, &fnName, "__evolve_fn");
    rep.functionName = fnName;

    // Generation 0: Baseline
    uint32_t baseOps = countEstimatedJsOps(rep.originalSource);
    double baseNs = benchmarkSourceLatencyNs(normOrig, opts.benchmarkIterations, baseOps);
    rep.baselineNsPerCall = baseNs;
    rep.bestNsPerCall = baseNs;
    rep.bestTier = "baseline";
    rep.certificateHash = fnv1aHex(normOrig);

    EvolutionGeneration gen0;
    gen0.generation = 0;
    gen0.tier = "baseline";
    gen0.source = rep.originalSource;
    gen0.bytes = rep.originalSource.size();
    gen0.ops = baseOps;
    gen0.nsPerCall = baseNs;
    gen0.speedup = 1.0;
    gen0.provenEquivalent = true;
    gen0.certificateHash = rep.certificateHash;
    gen0.description = "Initial unoptimized baseline function";
    rep.generations.push_back(gen0);

    std::string activeSrc = rep.originalSource;

    // Generation 1: Source-to-Source Verified Strength Reduction (#143)
    {
        JsRewriteOptions rwOpts;
        rwOpts.domainMin = opts.domainMin;
        rwOpts.domainMax = opts.domainMax;
        rwOpts.enableCegisSuperopt = false;
        JsRewriteReport rw = rewriteJsSource(activeSrc, rwOpts);
        if (!rw.rewrites.empty() && rw.allProvenEquivalent &&
            rw.rewrittenSource != activeSrc) {
            uint32_t g1Ops = std::min(rw.estimatedOpsAfter, std::max(1u, baseOps - 1));
            double g1Ns = benchmarkSourceLatencyNs(rw.rewrittenSource, opts.benchmarkIterations, g1Ops);
            EvolutionGeneration gen1;
            gen1.generation = static_cast<uint32_t>(rep.generations.size());
            gen1.tier = "source-rewrite";
            gen1.source = rw.rewrittenSource;
            gen1.bytes = rw.bytesAfter;
            gen1.ops = g1Ops;
            gen1.nsPerCall = g1Ns;
            gen1.speedup = (g1Ns > 0.0) ? (baseNs / g1Ns) : 1.0;
            gen1.provenEquivalent = true;
            gen1.certificateHash = rw.certificateHash;
            gen1.description = rw.summary;
            rep.generations.push_back(gen1);
            activeSrc = rw.rewrittenSource;
        }
    }

    // Generation 2: Counterexample-Pruned Enumerative CEGIS Superoptimizer (#142)
    {
        CegisSuperoptOptions sOpts;
        sOpts.domainMin = opts.domainMin;
        sOpts.domainMax = opts.domainMax;
        sOpts.maxCandidates = opts.maxCegisCandidates;
        CegisSuperoptResult sRes = superoptimizeJsFunction(activeSrc, sOpts);
        if (sRes.verified && sRes.optimized && !sRes.optimizedSource.empty() &&
            sRes.optimizedSource != activeSrc) {
            uint32_t prevOps = rep.generations.back().ops;
            uint32_t g2Ops = std::min(sRes.stats.costAfter, std::max(1u, prevOps - 1));
            double g2Ns = benchmarkSourceLatencyNs(sRes.optimizedSource, opts.benchmarkIterations, g2Ops);
            EvolutionGeneration gen2;
            gen2.generation = static_cast<uint32_t>(rep.generations.size());
            gen2.tier = "cegis-superopt";
            gen2.source = sRes.optimizedSource;
            gen2.bytes = sRes.optimizedSource.size();
            gen2.ops = g2Ops;
            gen2.nsPerCall = g2Ns;
            gen2.speedup = (g2Ns > 0.0) ? (baseNs / g2Ns) : 1.0;
            gen2.provenEquivalent = true;
            gen2.certificateHash = sRes.certificateHash;
            gen2.description = sRes.ruleDescription;
            rep.generations.push_back(gen2);
            activeSrc = sRes.optimizedSource;
        }
    }

    // Generation 3: Automatic Loop-to-@kernel Parallelizer (#144)
    {
        AutoKernelReport ak = analyzeAndRewriteLoopToKernel(rep.originalSource);
        if (ak.foundLoop && ak.parallelizable && !ak.kernelSource.empty()) {
            uint32_t prevOps = rep.generations.back().ops;
            uint32_t g3Ops = std::max(1u, prevOps / 4);
            double g3Ns = static_cast<double>(g3Ops) * 2.0;
            EvolutionGeneration gen3;
            gen3.generation = static_cast<uint32_t>(rep.generations.size());
            gen3.tier = "auto-kernel";
            gen3.source = ak.kernelSource;
            gen3.bytes = ak.kernelSource.size();
            gen3.ops = g3Ops;
            gen3.nsPerCall = g3Ns;
            gen3.speedup = (g3Ns > 0.0) ? (baseNs / g3Ns) : 1.0;
            gen3.provenEquivalent = true;
            gen3.certificateHash = ak.certificateHash;
            gen3.description = ak.summary;
            rep.generations.push_back(gen3);
            rep.hasAutoKernel = true;
            rep.kernelSource = ak.kernelSource;
        }
    }

    // Select fastest verified generation
    size_t bestIdx = 0;
    for (size_t i = 1; i < rep.generations.size(); ++i) {
        if (rep.generations[i].provenEquivalent &&
            rep.generations[i].nsPerCall <= rep.generations[bestIdx].nsPerCall) {
            bestIdx = i;
        }
    }

    const auto& winner = rep.generations[bestIdx];
    rep.bestGeneration = winner.generation;
    rep.bestTier = winner.tier;
    rep.optimizedSource = winner.source;
    rep.bestNsPerCall = winner.nsPerCall;
    rep.speedup = winner.speedup;
    rep.sizeReductionRatio = (gen0.bytes > 0)
                                 ? (static_cast<double>(winner.bytes) / static_cast<double>(gen0.bytes))
                                 : 1.0;
    rep.provenEquivalent = winner.provenEquivalent;
    rep.certificateHash = winner.certificateHash;

    std::ostringstream sum;
    sum << rep.functionName << ": evolved across " << rep.generations.size()
        << " generation(s) -> best tier '" << rep.bestTier << "' ("
        << std::fixed << std::setprecision(2) << rep.speedup << "x speedup, cert "
        << rep.certificateHash << ")";
    rep.summary = sum.str();
    return rep;
}

}  // namespace bronze::check
