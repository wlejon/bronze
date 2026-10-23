#pragma once

#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "check/synth_tests.h"

namespace bronze::check {

struct ProveOptions {
    int32_t domainMin = INT32_MIN;
    int32_t domainMax = INT32_MAX;
    std::string equivalentToSource;
    std::string inverseFnSource;
    bool synthesizeTests = true;
    bool strictDomNullability = true;
    std::string fnName;
};

struct ProofCertificateReport {
    bool verified = false;
    bool hazardFree = false;
    bool deterministic = false;
    bool proofsValid = false;
    uint32_t provenGuards = 0;
    uint32_t totalGuards = 0;
    uint32_t unboxedF64Ops = 0;
    bool hasEquivalenceCheck = false;
    bool equivalent = true;
    bool hasRoundTripCheck = false;
    bool roundTripProven = true;
    std::vector<int64_t> counterexampleArgs;
    int64_t expectedOutput = 0;
    int64_t actualOutput = 0;
    std::vector<std::string> hazards;
    std::vector<std::string> lockstepHazards;
    std::vector<SynthesizedTestCase> synthesizedTests;
    std::string sha256;
    std::string certificateJson;
    std::string error;
};

// Runs the unified Bronze formal verification & proof-certificate suite on an
// in-memory JS function:
//   1. Static symbolic hazard check (`checkModule`)
//   2. Cross-platform deterministic lockstep audit (`checkDeterministicLockstep`)
//   3. Zero-guard IL proof certificate verification (`il::verifyModuleProofs`)
//   4. Optional bit-exact equivalence proof (`proveSourceEquivalent`)
//   5. Optional codec round-trip proof (`proveRoundTripSource`)
//   6. Optional concolic path test synthesis (`synthesizeModuleTests`)
ProofCertificateReport proveJsFunction(std::string_view fnSource,
                                       const ProveOptions& opts = {});

}  // namespace bronze::check
