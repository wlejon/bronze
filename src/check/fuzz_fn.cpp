#include "check/fuzz_fn.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "check/solve_fn.h"
#include "check/static_checker.h"
#include "check/synth_tests.h"

namespace bronze::check {
namespace {

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

std::string classifyHazardDescription(std::string_view desc) {
    if (desc.find("div") != std::string_view::npos || desc.find("zero") != std::string_view::npos ||
        desc.find("modulo") != std::string_view::npos) {
        return "div-by-zero";
    }
    if (desc.find("bounds") != std::string_view::npos || desc.find("index") != std::string_view::npos ||
        desc.find("OOB") != std::string_view::npos) {
        return "out-of-bounds";
    }
    if (desc.find("null") != std::string_view::npos || desc.find("undefined") != std::string_view::npos) {
        return "null-deref";
    }
    if (desc.find("NaN") != std::string_view::npos || desc.find("domain") != std::string_view::npos) {
        return "nan-poison";
    }
    if (desc.find("throw") != std::string_view::npos || desc.find("exception") != std::string_view::npos) {
        return "exception";
    }
    return "hazard";
}

}  // namespace

std::string FuzzAuditReport::renderJson() const {
    std::ostringstream os;
    os << "{"
       << "\"functionName\":\"" << jsonEscape(functionName) << "\","
       << "\"branchCoverage\":" << branchCoverage << ","
       << "\"totalBranches\":" << totalBranches << ","
       << "\"coveredBranches\":" << coveredBranches << ","
       << "\"safe\":" << (safe ? "true" : "false") << ","
       << "\"testVectors\":[";
    for (size_t i = 0; i < testVectors.size(); ++i) {
        if (i > 0) os << ",";
        const auto& tv = testVectors[i];
        os << "{\"input\":[";
        for (size_t j = 0; j < tv.args.size(); ++j) {
            if (j > 0) os << ",";
            os << tv.args[j];
        }
        os << "],\"output\":" << tv.returnValue
           << ",\"block\":" << tv.targetBlockId
           << ",\"path\":\"" << jsonEscape(tv.pathDescription) << "\"}";
    }
    os << "],\"hazards\":[";
    for (size_t i = 0; i < hazards.size(); ++i) {
        if (i > 0) os << ",";
        const auto& h = hazards[i];
        os << "{\"kind\":\"" << jsonEscape(h.kind) << "\",\"witness\":[";
        for (size_t j = 0; j < h.witnessArgs.size(); ++j) {
            if (j > 0) os << ",";
            os << h.witnessArgs[j];
        }
        os << "],\"description\":\"" << jsonEscape(h.description) << "\"}";
    }
    os << "]}";
    return os.str();
}

FuzzAuditReport fuzzJsFunction(std::string_view fnSource, const FuzzOptions& opts) {
    FuzzAuditReport report;
    std::string detectedName;
    std::string defaultName = opts.fnName.empty() ? "__fuzz_target" : opts.fnName;
    std::string normalized = normalizeJsFunctionSource(fnSource, &detectedName, defaultName);
    report.functionName = !opts.fnName.empty() ? opts.fnName : detectedName;

    std::string synthErr;
    SynthTestsReport synth = synthesizeSourceTests(normalized, &synthErr);
    if (!synthErr.empty()) {
        report.error = synthErr;
        report.safe = false;
        return report;
    }

    report.totalBranches = synth.totalBlocks;
    report.coveredBranches = std::min(synth.coveredBlocks, synth.totalBlocks);
    if (report.totalBranches > 0) {
        report.branchCoverage =
            static_cast<double>(report.coveredBranches) / static_cast<double>(report.totalBranches);
    } else {
        report.branchCoverage = 1.0;
    }

    for (const auto& tc : synth.tests) {
        if (report.paramNames.empty() && !tc.paramNames.empty()) {
            report.paramNames = tc.paramNames;
        }
        if (tc.kind == SynthTestKind::BranchCoverage) {
            if (report.testVectors.size() >= opts.maxPaths) continue;
            FuzzTestVector vec;
            vec.args = tc.args;
            vec.targetBlockId = tc.targetBlockId;
            vec.pathDescription = tc.pathDescription;
            if (tc.hasExpectedReturn) {
                vec.returnValue = tc.expectedReturn;
                vec.hasReturnValue = true;
            } else {
                int64_t concreteRet = 0;
                if (evalConcreteJsFunction(normalized, tc.args, &concreteRet, nullptr, report.functionName)) {
                    vec.returnValue = concreteRet;
                    vec.hasReturnValue = true;
                }
            }
            report.testVectors.push_back(std::move(vec));
        } else if (tc.kind == SynthTestKind::HazardReproducer) {
            FuzzHazardEntry haz;
            haz.kind = classifyHazardDescription(tc.hazardDescription);
            haz.witnessArgs = tc.args;
            haz.description = tc.hazardDescription;
            report.hazards.push_back(std::move(haz));
        }
    }

    // Also run static abstract interpretation to catch any NullDeref / OutOfBounds /
    // NaNPoison / domain / shift hazards not already emitted as concrete reproducers.
    std::string staticErr;
    CheckReport staticRep = checkSource(normalized, opts.strictDomNullability, &staticErr);
    for (const auto& finding : staticRep.findings) {
        std::string kindTag = bugKindTag(finding.kind);
        bool alreadyCovered = false;
        for (const auto& existing : report.hazards) {
            if (existing.description == finding.message) {
                alreadyCovered = true;
                break;
            }
        }
        if (!alreadyCovered) {
            FuzzHazardEntry haz;
            haz.kind = kindTag;
            if (!report.testVectors.empty()) {
                haz.witnessArgs = report.testVectors.front().args;
            }
            haz.description = finding.message;
            SourceBuffer buf("fuzz.js", normalized);
            auto lc = buf.lineCol(finding.span.begin);
            haz.line = lc.line;
            haz.col = lc.column;
            report.hazards.push_back(std::move(haz));
        }
    }

    report.safe = report.hazards.empty();
    return report;
}

}  // namespace bronze::check
