#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::check {

struct FuzzOptions {
    uint32_t maxPaths = 64;
    bool strictDomNullability = true;
    std::string fnName;
};

struct FuzzTestVector {
    std::vector<int64_t> args;
    int64_t returnValue = 0;
    bool hasReturnValue = false;
    uint32_t targetBlockId = 0;
    std::string pathDescription;
};

struct FuzzHazardEntry {
    std::string kind;
    std::vector<int64_t> witnessArgs;
    std::string description;
    uint32_t line = 0;
    uint32_t col = 0;
};

struct FuzzAuditReport {
    std::string functionName;
    std::vector<std::string> paramNames;
    double branchCoverage = 1.0;
    uint32_t totalBranches = 0;
    uint32_t coveredBranches = 0;
    std::vector<FuzzTestVector> testVectors;
    std::vector<FuzzHazardEntry> hazards;
    bool safe = true;
    std::string error;

    std::string renderJson() const;
};

// White-box concolic bug hunter & 100% path-covering test generator.
// Combines `synthesizeSourceTests` (SMT path exploration + hazard witness solving),
// `checkSource` (abstract interpretation static hazard checker), and concrete
// witness execution to return a structured `FuzzAuditReport`.
FuzzAuditReport fuzzJsFunction(std::string_view fnSource,
                               const FuzzOptions& opts = {});

}  // namespace bronze::check
