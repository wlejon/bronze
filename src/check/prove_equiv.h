#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "il/il.h"
#include "modules/resolve.h"

namespace bronze::check {

enum class EquivStatus : uint8_t {
    Equivalent,      // Mathematically proven equivalent over all symbolic inputs
    Counterexample,  // Concrete input assignment found where return values diverge
    Unknown,         // Unbounded loop, external side effect, or solver resource limit
};

const char* equivStatusTag(EquivStatus status);

struct FunctionEquivResult {
    std::string name;
    std::vector<std::string> paramNames;
    EquivStatus status = EquivStatus::Unknown;
    std::vector<int64_t> counterexampleArgs;
    int64_t oldReturnValue = 0;
    int64_t newReturnValue = 0;
    std::string detail;
};

struct ProveEquivReport {
    std::vector<FunctionEquivResult> functions;

    bool empty() const { return functions.empty(); }
    bool allEquivalent() const;
    bool hasCounterexample() const;

    std::string renderText() const;
    std::string renderJson() const;
};

// Symbolically compares matching functions in `oldMod` and `newMod` by encoding
// both into a shared Brass bitvector SMT context (`brass::verify::BVContext`)
// and invoking CDCL bit-blasting (`brass::verify::solve_nonzero`) plus MIR
// equivalence verification (`brass::verify::verify_mir_equivalence`).
ProveEquivReport proveModuleEquiv(const il::Module& oldMod,
                                  const il::Module& newMod,
                                  const std::string& fnFilter = "");

// Parses, infers, and lowers two in-memory JS source strings (`oldSrc`,
// `newSrc`) and proves equivalence of their exported/top-level functions.
ProveEquivReport proveSourceEquiv(std::string_view oldSrc,
                                  std::string_view newSrc,
                                  const std::string& fnFilter = "",
                                  std::string* errorOut = nullptr);

// CLI entry point for `bronze prove-equiv <old.js> <new.js> [--fn <name>] [--json]`.
// Returns 0 when all compared functions are proven EQUIVALENT, 1 when a
// COUNTEREXAMPLE or error is found, and 2 when any function is UNKNOWN without
// counterexamples.
int runProveEquiv(const std::string& oldPath,
                  const std::string& newPath,
                  const std::string& fnFilter = "",
                  bool jsonOutput = false,
                  std::ostream& out = std::cout,
                  std::ostream& err = std::cerr,
                  const std::vector<modules::ModuleRoot>& moduleRoots = {},
                  const std::string& importMapPath = {});

}  // namespace bronze::check
