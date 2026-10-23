#pragma once

#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::check {

struct CounterexampleInfo {
    uint32_t round = 0;
    std::string candidateSource;
    std::vector<int64_t> args;
    int64_t expectedOutput = 0;
    int64_t actualOutput = 0;
};

struct OptimizeOptions {
    int32_t domainMin = -10000;
    int32_t domainMax = 10000;
    uint32_t maxRounds = 16;
    bool autoPatchCounterexamples = true;
    std::string fnName;
    std::vector<std::string> candidates;
    std::function<std::string(const std::string& failedCandidate,
                              const CounterexampleInfo& cex)> proposeCandidate;
};

struct OptimizeResult {
    bool verified = false;
    bool optimized = false;
    std::string referenceSource;
    std::string optimizedSource;
    uint32_t roundsUsed = 0;
    int32_t domainMin = -10000;
    int32_t domainMax = 10000;
    std::vector<CounterexampleInfo> counterexampleHistory;
    std::string summary;
};

// Runs Counterexample-Guided Inductive Synthesis (CEGIS) against `refSource`
// over `[opts.domainMin, opts.domainMax]`. Evaluates user-supplied candidates,
// callback-proposed candidates, built-in algebraic/strength-reduction templates,
// and automatic counterexample boundary guards until `proveSourceEquivalent`
// returns `EquivStatus::Equivalent`.
OptimizeResult optimizeJsFunction(std::string_view refSource,
                                  const OptimizeOptions& opts = {});

}  // namespace bronze::check
