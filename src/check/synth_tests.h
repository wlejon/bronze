#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "modules/resolve.h"

namespace bronze::check {

enum class SynthTestKind : uint8_t {
    BranchCoverage,    // Concrete input vector driving execution into a specific basic block / branch arm
    HazardReproducer,  // Concrete input vector triggering a static hazard (div-by-zero, OOB index, throw)
};

const char* synthTestKindTag(SynthTestKind kind);

struct SynthesizedTestCase {
    std::string functionName;
    std::vector<std::string> paramNames;
    std::vector<int64_t> args;
    int64_t expectedReturn = 0;
    bool hasExpectedReturn = false;
    SynthTestKind kind = SynthTestKind::BranchCoverage;
    uint32_t targetBlockId = 0;
    std::string pathDescription;
    std::string hazardDescription;
};

struct SynthTestsReport {
    std::string sourcePath;
    std::vector<SynthesizedTestCase> tests;
    uint32_t totalBlocks = 0;
    uint32_t coveredBlocks = 0;
    uint32_t hazardReproducerCount = 0;

    bool empty() const { return tests.empty(); }
    std::string renderJs() const;
    std::string renderJson() const;
};

// Symbolically explores all feasible paths and branch conditions of functions in
// `mod` using Brass's bitvector SMT solver (`brass::verify::solve_nonzero`) to
// synthesize concrete test inputs achieving maximal basic-block coverage as well
// as concrete reproducer inputs for runtime hazards.
SynthTestsReport synthesizeModuleTests(const il::Module& mod,
                                       const ast::Module* astMod = nullptr,
                                       const std::string& sourcePath = "");

// Convenience helper for synthesizing tests from an in-memory JS source string.
SynthTestsReport synthesizeSourceTests(std::string_view src,
                                       std::string* errorOut = nullptr);

inline SynthTestsReport synthesizeTestsForSource(std::string_view src,
                                                 std::string* errorOut = nullptr) {
    return synthesizeSourceTests(src, errorOut);
}

// CLI entry point for `bronze synth-tests <file.js> [-o <out.js>] [--json]`.
// Returns 0 on success and 1 on load/parse error or file write error.
int runSynthTests(const std::string& inputPath,
                  const std::string& outputPath = "",
                  bool jsonOutput = false,
                  std::ostream& out = std::cout,
                  std::ostream& err = std::cerr,
                  const std::vector<modules::ModuleRoot>& moduleRoots = {},
                  const std::string& importMapPath = {});

}  // namespace bronze::check
