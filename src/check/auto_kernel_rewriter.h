#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bronze::check {

struct LoopDependencyWitness {
    // "RAW", "WAW", "WAR", "OUT_OF_BOUNDS" (solver witnesses), "SIDE_EFFECT",
    // "NON_AFFINE", "UNSUPPORTED" (rejected from the AST), "UNDECIDED" (the
    // solver ran out of budget: not proven, no witness).
    std::string hazardKind;
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
    // Pairs of array names the proof needs to be different arrays: as one
    // array, an iteration's write could meet another iteration's access.
    // Empty when the kernel is sound however the caller's arrays alias.
    std::vector<std::pair<std::string, std::string>> requiresDistinct;
    std::string summary;
    std::string error;

    std::string toJson() const;
};

// Parses a sequential JS function, finds its first canonical
// `for (let i = start; i < bound; i++)` loop in the AST (braced or not), and
// collects every element read and write of the WHOLE body (nested blocks
// included) with its index as an affine form over `i`, the parameters and
// constants (auto_kernel_loop.h). Calls, allocations, property writes, writes
// to outer bindings and early exits reject the loop. `brass::verify::BVSolver`
// then proves, over two symbolic iterations `i1 != i2` in [start, bound), the
// absence of WAW/RAW/WAR loop-carried dependencies and of negative (and,
// against a `p.length` the loop names, past-the-end) indices. The emitted
// `// @kernel` is the body's own source text under a `global_tid_x()` guard,
// spliced only after it re-parses to the tree that was analysed.
// Array names are proven as distinct arrays, and again as one array:
// `requiresDistinct` lists the pairs the kernel needs to differ. Assumed, not
// proven: scalars and iterations are int32 numbers.
AutoKernelReport analyzeAndRewriteLoopToKernel(std::string_view source);

// CLI entry point for `bronze auto-kernel <file.js> [--diff] [--json] [--write]`.
int runAutoKernelCli(int argc, char** argv);

}  // namespace bronze::check
