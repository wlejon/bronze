#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "check/cegis_superopt.h"

namespace bronze::check {

struct JsRewriteOptions {
    int32_t domainMin = -10000;
    int32_t domainMax = 10000;
    bool enableDeadGuardElim = true;
    bool enableStrengthReduction = true;
    bool enableHorner = true;
    bool enableLoopDeforestation = true;
    bool enableCegisSuperopt = true;
    bool requireFormalProof = true;
};

struct JsRewritePassHit {
    std::string functionName;
    std::string ruleName;
    std::string description;
    std::string beforeSnippet;
    std::string afterSnippet;
    bool provenEquivalent = false;
    std::string certificateHash;
};

struct JsRewriteReport {
    std::string originalSource;
    std::string rewrittenSource;
    std::string unifiedDiff;
    std::vector<JsRewritePassHit> rewrites;
    size_t bytesBefore = 0;
    size_t bytesAfter = 0;
    uint32_t estimatedOpsBefore = 0;
    uint32_t estimatedOpsAfter = 0;
    bool allProvenEquivalent = true;
    std::string certificateHash;
    CegisSuperoptStats cegisStats;
    std::string summary;
    std::string error;

    std::string toJson() const;
};

// Rewrites JavaScript source code (`source`) for size and speed using:
// 1. SMT Dead-Branch & Redundant Guard Elimination
// 2. Polymorphic Idiom Strength Reduction (`Math.floor(x / 2^k)`, `Math.pow(x, 2/3)`, `% 2^k`)
// 3. Horner's Method Polynomial Factoring (`a*x^3 + b*x^2 + c*x + d` -> `((a*x + b)*x + c)*x + d`)
// 4. Loop Deforestation (`.map().filter().reduce()` -> 1 zero-alloc loop) & Closed-Form Series
// 5. Enumerative CEGIS Superoptimization (branchless bit-hacks)
// Every numeric/bitwise function rewrite is gated by `proveSourceEquivalent`.
JsRewriteReport rewriteJsSource(std::string_view source,
                                const JsRewriteOptions& opts = {});

// CLI entry point for `bronze optimize <file.js> [--write] [--diff] [--json]`.
int runOptimizeCli(int argc, char** argv);

}  // namespace bronze::check
