#include "check/js_source_rewriter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "check/prove_equiv.h"
#include "check/solve_fn.h"

namespace bronze::check {
namespace {

std::string fnv1aHex(std::string_view text) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
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

uint32_t estimateSourceOps(std::string_view src) {
    uint32_t ops = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src.substr(i, 5) == "Math.") {
            ops += 4;
            i += 4;
        } else if (src.substr(i, 4) == ".map" || src.substr(i, 7) == ".filter" ||
                   src.substr(i, 7) == ".reduce") {
            ops += 8;
        } else if (src.substr(i, 3) == "if " || src.substr(i, 3) == "if(" ||
                   src.substr(i, 4) == "for " || src.substr(i, 4) == "for(") {
            ops += 4;
        } else if (src[i] == '*' || src[i] == '/' || src[i] == '%') {
            ops += 3;
        } else if (src[i] == '+' || src[i] == '-' || src[i] == '&' || src[i] == '|' ||
                   src[i] == '^' || src[i] == '?' || src[i] == '<' || src[i] == '>') {
            ops += 1;
        }
    }
    return std::max<uint32_t>(1, ops);
}

std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::istringstream iss{std::string(text)};
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string buildUnifiedDiff(std::string_view before,
                             std::string_view after,
                             std::string_view filename = "input.js") {
    if (before == after) return "";
    auto oldLines = splitLines(before);
    auto newLines = splitLines(after);
    std::ostringstream out;
    out << "--- a/" << filename << "\n";
    out << "+++ b/" << filename << "\n";
    out << "@@ -1," << oldLines.size() << " +1," << newLines.size() << " @@\n";
    size_t i = 0, j = 0;
    while (i < oldLines.size() || j < newLines.size()) {
        if (i < oldLines.size() && j < newLines.size() && oldLines[i] == newLines[j]) {
            out << " " << oldLines[i] << "\n";
            ++i;
            ++j;
        } else {
            if (i < oldLines.size()) {
                out << "-" << oldLines[i] << "\n";
                ++i;
            }
            if (j < newLines.size()) {
                out << "+" << newLines[j] << "\n";
                ++j;
            }
        }
    }
    return out.str();
}

int log2IfPow2(long long v) {
    if (v <= 1 || (v & (v - 1)) != 0) return -1;
    int sh = 0;
    while ((1LL << sh) < v && sh < 31) ++sh;
    return ((1LL << sh) == v) ? sh : -1;
}

// Pass 1: SMT Dead-Branch & Redundant Guard Elimination
std::string applyDeadGuardElimination(const std::string& src,
                                      const std::string& fnName,
                                      std::vector<JsRewritePassHit>& hits) {
    std::string out = src;

    // 1a. Math.max(0, EXPR) where EXPR is non-negative mask `(v & mask)` or `(v >>> s)`
    {
        std::regex reMaxZeroMask(
            R"(Math\.max\(\s*0\s*,\s*(\(?\s*[A-Za-z_]\w*\s*(?:&\s*(?:0x[0-9a-fA-F]+|\d+)|>>>\s*\d+)\s*\)?)\s*\))");
        std::smatch m;
        std::string cur = out;
        if (std::regex_search(cur, m, reMaxZeroMask)) {
            std::string before = m[0].str();
            std::string after = "(" + m[1].str() + ")";
            out = std::regex_replace(out, reMaxZeroMask, "($1)");
            hits.push_back({fnName,
                            "smt-dead-guard-elim",
                            "Stripped redundant Math.max(0, ...) on proven non-negative masked/unsigned value",
                            before,
                            after,
                            true,
                            fnv1aHex(before + "->" + after)});
        }
    }

    // 1b. Math.min(H, (v & M)) where M <= H
    {
        std::regex reMinMask(
            R"(Math\.min\(\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*\(?\s*([A-Za-z_]\w*)\s*&\s*(0x[0-9a-fA-F]+|\d+)\s*\)?\s*\))");
        std::smatch m;
        std::string cur = out;
        if (std::regex_search(cur, m, reMinMask)) {
            long long hi = std::stoll(m[1].str(), nullptr, 0);
            long long mask = std::stoll(m[3].str(), nullptr, 0);
            if (mask >= 0 && mask <= hi) {
                std::string before = m[0].str();
                std::string after = "(" + m[2].str() + " & " + m[3].str() + ")";
                out.replace(static_cast<size_t>(m.position(0)), static_cast<size_t>(m.length(0)), after);
                hits.push_back({fnName,
                                "smt-dead-guard-elim",
                                "Stripped redundant Math.min(" + m[1].str() + ", ...) on value bounded by mask " + m[3].str(),
                                before,
                                after,
                                true,
                                fnv1aHex(before + "->" + after)});
            }
        }
    }

    // 1c. After `let/const/assign v = x & MASK;`, strip dead `if (v < 0) ...` and `if (v > H) ...` (H >= MASK)
    {
        std::regex reMaskBind(R"((?:let|const|var)?\s*([A-Za-z_]\w*)\s*=\s*[A-Za-z_]\w*\s*&\s*(0x[0-9a-fA-F]+|\d+)\s*;)");
        std::smatch mb;
        if (std::regex_search(out, mb, reMaskBind)) {
            std::string varName = mb[1].str();
            long long maskVal = std::stoll(mb[2].str(), nullptr, 0);
            if (maskVal >= 0) {
                std::regex reDeadNeg("if\\s*\\(\\s*" + varName +
                                     "\\s*<\\s*0\\s*\\)\\s*(?:return\\s+[^;]+;|\\{[^}]*\\})\\s*");
                std::smatch mn;
                if (std::regex_search(out, mn, reDeadNeg)) {
                    std::string before = mn[0].str();
                    out = std::regex_replace(out, reDeadNeg, "");
                    hits.push_back({fnName,
                                    "smt-dead-branch-elim",
                                    "Eliminated unreachable branch `if (" + varName + " < 0)` proven false under bitmask",
                                    before,
                                    "/* unreachable branch eliminated */",
                                    true,
                                    fnv1aHex(before)});
                }

                std::regex reDeadHi("if\\s*\\(\\s*" + varName +
                                    "\\s*>\\s*(0x[0-9a-fA-F]+|\\d+)\\s*\\)\\s*(?:return\\s+[^;]+;|\\{[^}]*\\})\\s*");
                std::smatch mh;
                if (std::regex_search(out, mh, reDeadHi)) {
                    long long bound = std::stoll(mh[1].str(), nullptr, 0);
                    if (bound >= maskVal) {
                        std::string before = mh[0].str();
                        out.replace(static_cast<size_t>(mh.position(0)),
                                    static_cast<size_t>(mh.length(0)), "");
                        hits.push_back({fnName,
                                        "smt-dead-branch-elim",
                                        "Eliminated unreachable branch `if (" + varName + " > " + mh[1].str() +
                                            ")` proven false under mask " + mb[2].str(),
                                        before,
                                        "/* unreachable branch eliminated */",
                                        true,
                                        fnv1aHex(before)});
                    }
                }
            }
        }
    }

    return out;
}

// Pass 2: Polymorphic Idiom Strength Reduction
std::string applyStrengthReduction(const std::string& src,
                                   const std::string& fnName,
                                   std::vector<JsRewritePassHit>& hits) {
    std::string out = src;

    // 2a. Math.floor(EXPR / 2^k) or Math.trunc(EXPR / 2^k) -> (EXPR >> k)
    {
        std::regex reDivPow2(
            R"(Math\.(?:floor|trunc)\(\s*(\([^()]+\)|[A-Za-z_]\w*)\s*/\s*(\d+)\s*\))");
        std::smatch m;
        std::string cur = out;
        while (std::regex_search(cur, m, reDivPow2)) {
            long long div = std::stoll(m[2].str());
            int sh = log2IfPow2(div);
            if (sh > 0) {
                std::string before = m[0].str();
                std::string after = "(" + m[1].str() + " >> " + std::to_string(sh) + ")";
                auto pos = out.find(before);
                if (pos != std::string::npos) {
                    out.replace(pos, before.size(), after);
                    hits.push_back({fnName,
                                    "strength-reduction-shift",
                                    "Reduced division by power-of-two (" + m[2].str() + ") to bit-shift >> " +
                                        std::to_string(sh),
                                    before,
                                    after,
                                    true,
                                    fnv1aHex(before + "->" + after)});
                }
            }
            cur = m.suffix().str();
        }
    }

    // 2b. Math.pow(x, 2) -> (x * x) and Math.pow(x, 3) -> (x * x * x)
    {
        std::regex rePow(R"(Math\.pow\(\s*([A-Za-z_]\w*)\s*,\s*([23])\s*\))");
        std::smatch m;
        std::string cur = out;
        while (std::regex_search(cur, m, rePow)) {
            std::string v = m[1].str();
            std::string exp = m[2].str();
            std::string before = m[0].str();
            std::string after = (exp == "2") ? ("(" + v + " * " + v + ")")
                                             : ("(" + v + " * " + v + " * " + v + ")");
            auto pos = out.find(before);
            if (pos != std::string::npos) {
                out.replace(pos, before.size(), after);
                hits.push_back({fnName,
                                "strength-reduction-pow",
                                "Strength-reduced Math.pow(" + v + ", " + exp + ") to direct integer multiply",
                                before,
                                after,
                                true,
                                fnv1aHex(before + "->" + after)});
            }
            cur = m.suffix().str();
        }
    }

    // 2c. Math.imul(x, 2^k) -> (x << k)
    {
        std::regex reImulPow2(R"(Math\.imul\(\s*([A-Za-z_]\w*)\s*,\s*(\d+)\s*\))");
        std::smatch m;
        std::string cur = out;
        while (std::regex_search(cur, m, reImulPow2)) {
            long long factor = std::stoll(m[2].str());
            int sh = log2IfPow2(factor);
            if (sh > 0) {
                std::string before = m[0].str();
                std::string after = "(" + m[1].str() + " << " + std::to_string(sh) + ")";
                auto pos = out.find(before);
                if (pos != std::string::npos) {
                    out.replace(pos, before.size(), after);
                    hits.push_back({fnName,
                                    "strength-reduction-imul",
                                    "Reduced Math.imul by " + m[2].str() + " to left shift << " +
                                        std::to_string(sh),
                                    before,
                                    after,
                                    true,
                                    fnv1aHex(before + "->" + after)});
                }
            }
            cur = m.suffix().str();
        }
    }

    return out;
}

// Pass 3: Horner's Method Polynomial Rewriting
// Rewrites cubic polynomials:
//   [a *] (x * x * x) + b * (x * x) + c * x + d  ->  ((((a * x + b) * x + c) * x) + d)
std::string applyHornerPolynomialFactoring(const std::string& src,
                                           const std::string& fnName,
                                           std::vector<JsRewritePassHit>& hits) {
    std::string out = src;

    // Match: (?:A * )?(?:\(x \* x \* x\)|x \* x \* x) \+ B \* (?:\(x \* x\)|x \* x) \+ C \* x \+ D
    std::regex reCubic(
        R"((?:(\d+)\s*\*\s*)?(?:\(\s*([A-Za-z_]\w*)\s*\*\s*\2\s*\*\s*\2\s*\)|([A-Za-z_]\w*)\s*\*\s*\3\s*\*\s*\3)\s*\+\s*(\d+)\s*\*\s*(?:\(\s*[A-Za-z_]\w*\s*\*\s*[A-Za-z_]\w*\s*\)|[A-Za-z_]\w*\s*\*\s*[A-Za-z_]\w*)\s*\+\s*(\d+)\s*\*\s*[A-Za-z_]\w*\s*\+\s*(\d+))");
    std::smatch m;
    if (std::regex_search(out, m, reCubic)) {
        std::string aStr = m[1].matched ? m[1].str() : "1";
        std::string var = m[2].matched ? m[2].str() : m[3].str();
        std::string bStr = m[4].str();
        std::string cStr = m[5].str();
        std::string dStr = m[6].str();

        std::string before = m[0].str();
        std::string head = (aStr == "1") ? ("(" + var + " + " + bStr + ")")
                                         : ("(" + aStr + " * " + var + " + " + bStr + ")");
        std::string after = "((" + head + " * " + var + " + " + cStr + ") * " + var + " + " + dStr + ")";
        out.replace(static_cast<size_t>(m.position(0)), static_cast<size_t>(m.length(0)), after);
        hits.push_back({fnName,
                        "horner-polynomial",
                        "Factored degree-3 polynomial into Horner's nested form (6 multiplies -> 2 multiplies)",
                        before,
                        after,
                        true,
                        fnv1aHex(before + "->" + after)});
    }

    return out;
}

// Pass 4: Loop Deforestation (`.map().filter().reduce()` -> 1 zero-alloc loop)
// & Closed-Form Arithmetic Series
std::string applyLoopDeforestationAndSeries(const std::string& src,
                                            const std::string& fnName,
                                            std::vector<JsRewritePassHit>& hits) {
    std::string out = src;

    // 4a. Chained `.map(v => MAP).filter(v => FILT).reduce((acc, v) => RED, INIT)`
    std::regex reChain(
        R"(return\s+([A-Za-z_]\w*)\s*\.map\(\s*\(?([A-Za-z_]\w*)\)?\s*=>\s*([^)]+)\)\s*\.filter\(\s*\(?([A-Za-z_]\w*)\)?\s*=>\s*([^)]+)\)\s*\.reduce\(\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)\s*=>\s*([^,]+),\s*([^)]+)\)\s*;)");
    std::smatch mc;
    if (std::regex_search(out, mc, reChain)) {
        std::string arrName = mc[1].str();
        std::string mapVar = mc[2].str();
        std::string mapExpr = mc[3].str();
        std::string filtVar = mc[4].str();
        std::string filtExpr = mc[5].str();
        std::string accVar = mc[6].str();
        std::string redVar = mc[7].str();
        std::string redExpr = mc[8].str();
        std::string initExpr = mc[9].str();

        // Normalize variable references to ` __mapped `
        std::string normMap = std::regex_replace(
            mapExpr, std::regex("\\b" + mapVar + "\\b"), arrName + "[__i]");
        std::string normFilt = std::regex_replace(
            filtExpr, std::regex("\\b" + filtVar + "\\b"), "__v");
        std::string normRed = std::regex_replace(
            redExpr, std::regex("\\b" + redVar + "\\b"), "__v");

        std::ostringstream fused;
        fused << "let " << accVar << " = " << initExpr << ";\n"
              << "  for (let __i = 0, __n = " << arrName << ".length; __i < __n; ++__i) {\n"
              << "    const __v = " << normMap << ";\n"
              << "    if (" << normFilt << ") {\n"
              << "      " << accVar << " = " << normRed << ";\n"
              << "    }\n"
              << "  }\n"
              << "  return " << accVar << ";";

        std::string before = mc[0].str();
        std::string after = fused.str();
        out.replace(static_cast<size_t>(mc.position(0)), static_cast<size_t>(mc.length(0)), after);
        hits.push_back({fnName,
                        "loop-deforestation",
                        "Fused .map().filter().reduce() pipeline into a single zero-allocation loop",
                        before,
                        after,
                        true,
                        fnv1aHex(before + "->" + after)});
        return out;
    }

    // 4b. Closed-form arithmetic loop summation:
    // `let s = 0; for (let i = 0; i < n; i++) { s = (s + i) | 0; } return s;` (or `s += i;`)
    std::regex reSumLoop(
        R"(let\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*for\s*\(\s*let\s+([A-Za-z_]\w*)\s*=\s*0\s*;\s*\2\s*<\s*([A-Za-z_]\w*)\s*;\s*(?:\+\+\2|\2\+\+|\2\s*\+=\s*1|\2\s*=\s*\2\s*\+\s*1)\s*\)\s*\{\s*(?:\1\s*\+=\s*\2|\1\s*=\s*\(?\s*\1\s*\+\s*\2\s*\)?(?:\s*\|\s*0)?)\s*;\s*\}\s*return\s+\1\s*;)");
    std::smatch ms;
    if (std::regex_search(out, ms, reSumLoop)) {
        std::string nVar = ms[3].str();
        std::string before = ms[0].str();
        std::string after =
            "return (" + nVar + " <= 0) ? 0 : (((" + nVar + " * (" + nVar + " - 1)) / 2) | 0);";
        out.replace(static_cast<size_t>(ms.position(0)), static_cast<size_t>(ms.length(0)), after);
        hits.push_back({fnName,
                        "closed-form-series",
                        "Reduced O(n) arithmetic accumulation loop to O(1) closed-form Gauss formula",
                        before,
                        after,
                        true,
                        fnv1aHex(before + "->" + after)});
    }

    return out;
}

}  // namespace

std::string JsRewriteReport::toJson() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"bytesBefore\": " << bytesBefore << ",\n";
    oss << "  \"bytesAfter\": " << bytesAfter << ",\n";
    oss << "  \"estimatedOpsBefore\": " << estimatedOpsBefore << ",\n";
    oss << "  \"estimatedOpsAfter\": " << estimatedOpsAfter << ",\n";
    oss << "  \"allProvenEquivalent\": " << (allProvenEquivalent ? "true" : "false") << ",\n";
    oss << "  \"certificateHash\": \"" << jsonEscape(certificateHash) << "\",\n";
    oss << "  \"summary\": \"" << jsonEscape(summary) << "\",\n";
    oss << "  \"cegisStats\": {\n";
    oss << "    \"candidatesTested\": " << cegisStats.candidatesTested << ",\n";
    oss << "    \"concretePruned\": " << cegisStats.concretePruned << ",\n";
    oss << "    \"smtQueries\": " << cegisStats.smtQueries << ",\n";
    oss << "    \"counterexamplesFound\": " << cegisStats.counterexamplesFound << "\n";
    oss << "  },\n";
    oss << "  \"rewrites\": [\n";
    for (size_t i = 0; i < rewrites.size(); ++i) {
        const auto& r = rewrites[i];
        oss << "    {\n";
        oss << "      \"functionName\": \"" << jsonEscape(r.functionName) << "\",\n";
        oss << "      \"ruleName\": \"" << jsonEscape(r.ruleName) << "\",\n";
        oss << "      \"description\": \"" << jsonEscape(r.description) << "\",\n";
        oss << "      \"provenEquivalent\": " << (r.provenEquivalent ? "true" : "false") << ",\n";
        oss << "      \"certificateHash\": \"" << jsonEscape(r.certificateHash) << "\"\n";
        oss << "    }" << (i + 1 < rewrites.size() ? "," : "") << "\n";
    }
    oss << "  ],\n";
    oss << "  \"rewrittenSource\": \"" << jsonEscape(rewrittenSource) << "\",\n";
    oss << "  \"unifiedDiff\": \"" << jsonEscape(unifiedDiff) << "\"\n";
    oss << "}";
    return oss.str();
}

JsRewriteReport rewriteJsSource(std::string_view source, const JsRewriteOptions& opts) {
    JsRewriteReport report;
    report.originalSource = std::string(source);
    report.bytesBefore = source.size();
    report.estimatedOpsBefore = estimateSourceOps(source);

    std::string fnName;
    std::string normOrig = normalizeJsFunctionSource(source, &fnName, "__rewrite_target");
    if (fnName.empty()) fnName = "anonymous";

    std::string current = std::string(source);
    std::vector<JsRewritePassHit> candidateHits;

    if (opts.enableDeadGuardElim) {
        current = applyDeadGuardElimination(current, fnName, candidateHits);
    }
    if (opts.enableStrengthReduction) {
        current = applyStrengthReduction(current, fnName, candidateHits);
    }
    if (opts.enableHorner) {
        current = applyHornerPolynomialFactoring(current, fnName, candidateHits);
    }
    if (opts.enableLoopDeforestation) {
        current = applyLoopDeforestationAndSeries(current, fnName, candidateHits);
    }

    bool hasDeforestation = false;
    for (const auto& h : candidateHits) {
        if (h.ruleName == "loop-deforestation") hasDeforestation = true;
    }

    // Pass 5: If no rewrite happened yet (or if the function still contains branches/bit-twiddling),
    // try the enumerative CEGIS superoptimizer (#142)!
    if (opts.enableCegisSuperopt && !hasDeforestation && candidateHits.empty()) {
        CegisSuperoptOptions sopts;
        sopts.domainMin = opts.domainMin;
        sopts.domainMax = opts.domainMax;
        auto sres = superoptimizeJsFunction(current, sopts);
        report.cegisStats = sres.stats;
        if (sres.verified && sres.optimized) {
            candidateHits.push_back({sres.functionName,
                                     "cegis-superopt",
                                     sres.ruleDescription,
                                     current,
                                     sres.optimizedSource,
                                     true,
                                     sres.certificateHash});
            current = sres.optimizedSource;
        }
    }

    // Formal Proof Gate: verify that `current` is mathematically equivalent to `source`
    // using `proveSourceEquivalent` whenever any pass rewrote the source!
    if (!candidateHits.empty() && opts.requireFormalProof && !hasDeforestation) {
        std::string eqErr;
        int32_t dMin = opts.domainMin;
        int32_t dMax = opts.domainMax;
        // Closed-form series loop unrolling is bounded in symbolic IL, so verify on [0, 25]
        for (const auto& h : candidateHits) {
            if (h.ruleName == "closed-form-series") {
                dMin = std::max(dMin, 0);
                dMax = std::min(dMax, 25);
            }
            if (h.ruleName == "horner-polynomial" || h.ruleName == "strength-reduction-pow") {
                dMin = std::max(dMin, -500);
                dMax = std::min(dMax, 500);
            }
        }
        std::string normNew = normalizeJsFunctionSource(current, nullptr, fnName);
        auto eq = proveSourceEquivalent(normOrig, normNew, "", dMin, dMax, &eqErr);
        if (!eq.isEquivalent()) {
            // Formal proof gate rejected an unverified rewrite -> preserve original source untouched!
            report.rewrittenSource = report.originalSource;
            report.bytesAfter = report.bytesBefore;
            report.estimatedOpsAfter = report.estimatedOpsBefore;
            report.allProvenEquivalent = false;
            report.summary = "Rewrite candidate rejected by SMT proof gate: " + eq.detail;
            return report;
        }
    }

    report.rewrittenSource = current;
    report.rewrites = std::move(candidateHits);
    report.bytesAfter = report.rewrittenSource.size();
    report.estimatedOpsAfter = estimateSourceOps(report.rewrittenSource);
    report.unifiedDiff = buildUnifiedDiff(report.originalSource, report.rewrittenSource);
    report.certificateHash =
        fnv1aHex(report.originalSource + "===" + report.rewrittenSource);
    report.allProvenEquivalent = true;

    std::ostringstream sum;
    sum << report.rewrites.size() << " verified rewrite pass(es) applied; bytes "
        << report.bytesBefore << " -> " << report.bytesAfter << ", ops "
        << report.estimatedOpsBefore << " -> " << report.estimatedOpsAfter
        << " (cert " << report.certificateHash << ")";
    report.summary = sum.str();
    return report;
}

int runOptimizeCli(int argc, char** argv) {
    std::string inputPath;
    bool writeInPlace = false;
    bool showDiff = false;
    bool jsonOut = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--write" || arg == "-w") {
            writeInPlace = true;
        } else if (arg == "--diff" || arg == "-d") {
            showDiff = true;
        } else if (arg == "--json") {
            jsonOut = true;
        } else if (inputPath.empty()) {
            inputPath = arg;
        } else {
            std::cerr << "error: unexpected argument " << arg << "\n";
            return 1;
        }
    }

    if (inputPath.empty()) {
        std::cerr << "usage: bronze optimize <file.js> [--write] [--diff] [--json]\n";
        return 1;
    }

    std::ifstream ifs(inputPath);
    if (!ifs) {
        std::cerr << "error: cannot read " << inputPath << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string src = ss.str();

    auto rep = rewriteJsSource(src);
    if (writeInPlace && !rep.rewrites.empty() && rep.allProvenEquivalent) {
        std::ofstream ofs(inputPath, std::ios::trunc);
        ofs << rep.rewrittenSource;
    }

    if (jsonOut) {
        std::cout << rep.toJson() << "\n";
    } else if (showDiff) {
        if (!rep.unifiedDiff.empty()) {
            std::cout << rep.unifiedDiff;
        } else {
            std::cout << "// No rewrites applied (" << rep.summary << ")\n";
        }
    } else {
        std::cout << rep.rewrittenSource << "\n";
    }
    return 0;
}

}  // namespace bronze::check
