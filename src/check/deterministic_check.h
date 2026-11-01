#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "modules/resolve.h"
#include "support/source.h"

namespace bronze::ast {
using Program = Module;
}  // namespace bronze::ast

namespace bronze::check {

enum class DeterministicHazardKind : uint8_t {
    UnseededPrng,             // Math.random() without a deterministic seeded PRNG
    WallClockTime,            // Date.now(), new Date(), performance.now()
    ObjectIterationOrder,     // for..in, Object.keys/values/entries without deterministic .sort()
    ExternalStateAccess,     // Mutable outer/module/global state access or non-whitelisted host call
};

const char* deterministicHazardTag(DeterministicHazardKind kind);

struct CheckDiagnostic {
    DeterministicHazardKind kind = DeterministicHazardKind::UnseededPrng;
    Span span;
    std::string functionName;
    std::string message;
    std::string remediation;
};

// Audits `prog` and `ilMod` for replay non-determinism hazards: unseeded
// PRNGs, wall-clock timers, unsorted object key enumeration and access to
// state outside the function. Floating-point results are not audited: math is
// the platform's, so a tick replays the same on one machine and need not
// match another machine to the bit.
void checkDeterministicLockstep(const ast::Program& prog,
                                const il::Module& ilMod,
                                std::vector<CheckDiagnostic>& diags);

// Convenience helper for auditing in-memory JS source code.
bool checkDeterministicSource(std::string_view src,
                              std::vector<CheckDiagnostic>& diags,
                              std::string* errorOut = nullptr);

std::string renderDeterministicDiagnostics(const std::vector<CheckDiagnostic>& diags,
                                           const SourceSet& sources);
std::string renderDeterministicDiagnostics(const std::vector<CheckDiagnostic>& diags,
                                           const SourceBuffer& buffer);

// CLI entry point for `bronze check --deterministic <file.js>`.
// Returns 0 when deterministic lockstep verification passes with zero hazards,
// and 1 when hazards or parse/load errors occur.
int runDeterministicCheckFile(const std::string& sourcePath,
                              std::string* outString = nullptr,
                              const std::vector<modules::ModuleRoot>& moduleRoots = {},
                              const std::string& importMapPath = {});

}  // namespace bronze::check
