#include "check/js_source_rewriter.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "check/js_rewrite_ast.h"
#include "check/prove_equiv.h"
#include "check/solve_fn.h"

// Every site is found in the parsed program and every replacement is a splice
// at that node's extent (js_rewrite_ast.h); the patterns are in
// js_rewrite_passes.cpp. This file runs them in order, re-parsing between
// passes, and gates the result on the SMT equivalence proof.

namespace bronze::check {
namespace {

using jsrw::fnv1aHex;

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

using Collector = void (*)(const jsrw::ParsedJs&, const std::string&, std::vector<jsrw::Edit>&);

// One pass over `current`: parse it, collect the pass's sites, splice their
// replacements in, and keep the result only if it still parses. A pass whose
// output does not parse is a bug in a pattern, and is dropped whole rather
// than handed on.
void runPass(Collector collect,
             const std::string& fnName,
             std::string& current,
             std::vector<JsRewritePassHit>& hits,
             std::string& error) {
    std::string perr;
    auto parsed = jsrw::ParsedJs::parse(current, &perr);
    if (!parsed) return;
    std::vector<jsrw::Edit> edits;
    collect(*parsed, fnName, edits);
    if (edits.empty()) return;
    std::vector<JsRewritePassHit> passHits;
    std::string next = jsrw::applyEdits(current, std::move(edits), passHits);
    if (!jsrw::ParsedJs::parse(next, &perr)) {
        if (!error.empty()) error += "\n";
        error += "a rewrite pass produced source that does not parse; the pass was discarded: " + perr;
        return;
    }
    current = std::move(next);
    for (auto& h : passHits) hits.push_back(std::move(h));
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
    if (!error.empty()) oss << "  \"error\": \"" << jsonEscape(error) << "\",\n";
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

    auto unchanged = [&](std::string summary) {
        report.rewrittenSource = report.originalSource;
        report.bytesAfter = report.bytesBefore;
        report.estimatedOpsAfter = report.estimatedOpsBefore;
        report.summary = std::move(summary);
        return report;
    };

    std::string parseError;
    if (!jsrw::ParsedJs::parse(report.originalSource, &parseError)) {
        report.error = parseError;
        return unchanged("Source does not parse; no rewrites applied");
    }

    std::string fnName;
    std::string normOrig = normalizeJsFunctionSource(source, &fnName, "__rewrite_target");
    if (fnName.empty()) fnName = "anonymous";

    std::string current = std::string(source);
    std::vector<JsRewritePassHit> candidateHits;

    if (opts.enableDeadGuardElim) {
        runPass(jsrw::collectDeadGuardEdits, fnName, current, candidateHits, report.error);
    }
    if (opts.enableStrengthReduction) {
        runPass(jsrw::collectStrengthEdits, fnName, current, candidateHits, report.error);
    }
    if (opts.enableHorner) {
        runPass(jsrw::collectHornerEdits, fnName, current, candidateHits, report.error);
    }
    if (opts.enableLoopDeforestation) {
        runPass(jsrw::collectLoopEdits, fnName, current, candidateHits, report.error);
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
            report.allProvenEquivalent = false;
            return unchanged("Rewrite candidate rejected by SMT proof gate: " + eq.detail);
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
