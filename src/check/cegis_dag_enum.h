#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bronze::check {

struct EnumTestVector {
    std::vector<int32_t> args;
    int32_t expected = 0;
};

struct SynthesizedDagCandidate {
    std::string expr;
    std::string description;
    uint32_t cost = 1;
    std::vector<int32_t> outputs;
};

struct DagEnumResult {
    std::vector<SynthesizedDagCandidate> matchingCandidates;
    uint32_t totalGenerated = 0;
    uint32_t uniqueEquivClasses = 0;
    uint32_t prunedByObsEquiv = 0;
};

// Bottom-up cost-tiered DAG enumerator with observational-equivalence pruning.
// Enumerates 32-bit JS bitwise/arithmetic DAGs in increasing cost order (1..maxCost),
// computing concrete output vectors across `bank` in O(|bank|) integer operations per node
// and immediately pruning expressions that share an observational signature with an
// already-discovered lower-or-equal-cost representative.
DagEnumResult enumerateBottomUpDags(const std::vector<std::string>& paramNames,
                                    const std::vector<int32_t>& extractedConsts,
                                    const std::vector<EnumTestVector>& bank,
                                    uint32_t maxCost = 4,
                                    uint32_t maxEnumeratedNodes = 1500,
                                    uint32_t maxMatchingCandidates = 32);

}  // namespace bronze::check
