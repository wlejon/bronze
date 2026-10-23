#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::check {

// ---------------------------------------------------------------------------
// Issue #148: Self-Healing Proof-Gated Live-Reload Supervisor
// ---------------------------------------------------------------------------

struct LiveSupervisorOptions {
    int32_t domainMin = -10000;
    int32_t domainMax = 10000;
    bool autoHeal = true;
    bool checkEquivalenceWithPrevious = true;
};

struct DivergenceWitness {
    bool hasDivergence = false;
    std::vector<int64_t> args;
    int64_t oldOutput = 0;
    int64_t newOutput = 0;
};

struct GuardRepairAction {
    std::string hazardKind;
    std::string beforeSnippet;
    std::string afterSnippet;
    std::string description;
};

struct LiveReloadReport {
    bool accepted = false;
    bool selfHealed = false;
    uint32_t originalHazardCount = 0;
    uint32_t remainingHazardCount = 0;
    std::string functionName;
    std::string candidateSource;
    std::string healedSource;
    std::string activeSource;
    std::vector<std::string> originalHazards;
    std::vector<std::string> remainingHazards;
    std::vector<GuardRepairAction> repairs;
    bool hasPrevious = false;
    bool behaviorallyIdentical = false;
    DivergenceWitness divergenceWitness;
    std::string certificateHash;
    std::string summary;
};

// Audits `candidateSource` with `proveJsFunction` + `fuzzJsFunction`, automatically
// synthesizes mathematical domain/divisor/shift guards when hazards are detected,
// re-verifies that 0 hazards remain (`selfHealed = true`), and checks behavioral
// equivalence against `previousSource` (`proveSourceEquivalent`).
LiveReloadReport superviseLiveReload(std::string_view previousSource,
                                     std::string_view candidateSource,
                                     const LiveSupervisorOptions& opts = {});

// ---------------------------------------------------------------------------
// Issue #149: Autonomous Background Superoptimization & Kernel Evolution Engine
// ---------------------------------------------------------------------------

struct EvolutionOptions {
    int32_t domainMin = -1000;
    int32_t domainMax = 1000;
    uint32_t benchmarkIterations = 256;
    uint32_t maxCegisCandidates = 1500;
};

struct EvolutionGeneration {
    uint32_t generation = 0;
    std::string tier;
    std::string source;
    size_t bytes = 0;
    uint32_t ops = 0;
    double nsPerCall = 0.0;
    double speedup = 1.0;
    bool provenEquivalent = false;
    std::string certificateHash;
    std::string description;
};

struct EvolutionReport {
    std::string functionName;
    std::string originalSource;
    std::string optimizedSource;
    std::string bestTier;
    uint32_t bestGeneration = 0;
    double baselineNsPerCall = 0.0;
    double bestNsPerCall = 0.0;
    double speedup = 1.0;
    double sizeReductionRatio = 1.0;
    bool provenEquivalent = true;
    bool hasAutoKernel = false;
    std::string kernelSource;
    std::string certificateHash;
    std::string summary;
    std::vector<EvolutionGeneration> generations;
};

// Runs a multi-generation prover-backed evolution pipeline on `fnSource`:
//   Gen 0 (`baseline`)       -> Original function
//   Gen 1 (`source-rewrite`) -> Verified source-to-source strength reduction (#143)
//   Gen 2 (`cegis-superopt`) -> Counterexample-pruned enumerative CEGIS synthesis (#142)
//   Gen 3 (`auto-kernel`)    -> Loop-carried dependency proof & `@kernel` promotion (#144)
EvolutionReport evolveJsFunction(std::string_view fnSource,
                                 const EvolutionOptions& opts = {});

}  // namespace bronze::check
