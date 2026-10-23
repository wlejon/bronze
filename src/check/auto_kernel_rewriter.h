#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::check {

struct LoopDependencyWitness {
    std::string hazardKind;  // "RAW", "WAW", "WAR", "OUT_OF_BOUNDS", "SIDE_EFFECT"
    std::string arrayName;
    std::string writeIndexExpr;
    std::string readIndexExpr;
    int64_t iterA = 0;
    int64_t iterB = 0;
    int64_t conflictIndex = 0;
    std::string description;
};

struct AutoKernelReport {
    bool foundLoop = false;
    bool parallelizable = false;
    bool provenNoWaw = false;
    bool provenNoRaw = false;
    bool provenNoWar = false;
    bool provenInBounds = false;
    std::string functionName;
    std::vector<std::string> paramNames;
    std::string inductionVar;
    std::string boundExpr;
    std::vector<std::string> writtenArrays;
    std::vector<std::string> readArrays;
    std::string originalSource;
    std::string kernelSource;
    std::string unifiedDiff;
    std::string certificateHash;
    std::vector<LoopDependencyWitness> hazards;
    std::string summary;
    std::string error;

    std::string toJson() const;
};

// Scans a sequential JS function containing a `for (let i = 0; i < n; i++)` loop over
// TypedArrays, uses `brass::verify::BVSolver` over two symbolic loop iterations
// `i1 != i2` (`0 <= i1 < n`, `0 <= i2 < n`) to prove absence of WAW/RAW/WAR loop-carried
// dependencies and out-of-bounds accesses, and emits a verified `// @kernel` function.
AutoKernelReport analyzeAndRewriteLoopToKernel(std::string_view source);

// CLI entry point for `bronze auto-kernel <file.js> [--diff] [--json] [--write]`.
int runAutoKernelCli(int argc, char** argv);

}  // namespace bronze::check
