#pragma once

#include <climits>
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

    ProvenEquivalent = Equivalent,
    RefutedWithCounterexample = Counterexample,
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

struct EquivReport {
    EquivStatus status = EquivStatus::Unknown;
    std::string fnName;
    int32_t domainMin = INT32_MIN;
    int32_t domainMax = INT32_MAX;
    int64_t counterexampleInput = 0;
    std::vector<int64_t> counterexampleArgs;
    int64_t expectedOutput = 0;
    int64_t actualOutput = 0;
    int64_t encodedIntermediate = 0;
    std::string detail;
    std::vector<FunctionEquivResult> functions;

    bool isEquivalent() const { return status == EquivStatus::Equivalent; }
    bool allEquivalent() const { return status == EquivStatus::Equivalent; }
    bool hasCounterexample() const { return status == EquivStatus::Counterexample; }
};

// Internal lowering helper shared across prove_equiv translation units.
bool lowerSingleSourceForEquiv(std::string_view src,
                               std::string_view virtualName,
                               il::Module& outMod,
                               std::string* errorOut = nullptr);

// Symbolically compares a single pair of IL functions via Brass bitvector SMT.
FunctionEquivResult compareIlFunctionPair(const il::Module& oldMod,
                                          const il::Function& oldFn,
                                          const il::Module& newMod,
                                          const il::Function& newFn,
                                          bool strictJs32Bitwise = false,
                                          int32_t domainMin = INT32_MIN,
                                          int32_t domainMax = INT32_MAX);

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

// Proves equivalence of two JS functions under exact 32-bit ECMAScript bitwise
// semantics over inputs constrained to `[domainMin, domainMax]`.
EquivReport proveSourceEquivalent(std::string_view oldSrc,
                                  std::string_view newSrc,
                                  std::string_view fnFilter = "",
                                  int32_t domainMin = INT32_MIN,
                                  int32_t domainMax = INT32_MAX,
                                  std::string* errorOut = nullptr);

// Compiles a JS module containing both `encodeFnName(x)` and `decodeFnName(y)`,
// symbolically composes `decode(encode(x))` over 32-bit bitvectors constrained
// to `[domainMin, domainMax]`, and uses `brass::verify::BVSolver` to either
// mathematically prove `decode(encode(x)) === x` for all `x` in the domain or
// refute it with a concrete counterexample `x`.
EquivReport proveRoundTripSource(std::string_view source,
                                 std::string_view encodeFnName,
                                 std::string_view decodeFnName,
                                 int32_t domainMin = INT32_MIN,
                                 int32_t domainMax = INT32_MAX,
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
