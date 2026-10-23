#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "check/cegis_optimize.h"

namespace bronze::check {

struct CegisSuperoptOptions {
    int32_t domainMin = -10000;
    int32_t domainMax = 10000;
    uint32_t maxCandidates = 2048;
    uint32_t maxSmtQueries = 64;
    bool requireLowerCost = true;
};

struct CegisSuperoptStats {
    uint32_t candidatesGenerated = 0;
    uint32_t candidatesTested = 0;
    uint32_t concretePruned = 0;
    uint32_t smtQueries = 0;
    uint32_t counterexamplesFound = 0;
    uint32_t seedVectorCount = 0;
    uint32_t finalVectorCount = 0;
    uint32_t costBefore = 0;
    uint32_t costAfter = 0;
    double elapsedMs = 0.0;
};

struct CegisSuperoptResult {
    bool verified = false;
    bool optimized = false;
    std::string functionName;
    std::vector<std::string> paramNames;
    std::string originalSource;
    std::string synthesizedExpression;
    std::string optimizedSource;
    std::string ruleDescription;
    std::string certificateHash;
    std::string summary;
    std::string error;
    CegisSuperoptStats stats;
    std::vector<CounterexampleInfo> counterexamples;
};

// Enumerative Counterexample-Guided Inductive Synthesis (CEGIS) superoptimizer (#142).
// Evaluates candidate DAGs against a growing concrete test-vector bank `T` in nanoseconds,
// querying `brass::verify::BVSolver` only for candidates that match `T` on all concrete inputs.
CegisSuperoptResult superoptimizeJsFunction(std::string_view fnSource,
                                            const CegisSuperoptOptions& opts = {});

}  // namespace bronze::check
