#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "modules/modules.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::check {

enum class BugKind : uint8_t {
    NullDeref,      // Dereferencing or invoking a provably/maybe null or undefined value
    OutOfBounds,    // Array or TypedArray index provably < 0 or >= length
    NaNPoison,      // Undefined or provable NaN flowing into arithmetic / numeric sink
};

const char* bugKindTag(BugKind kind);

struct BugFinding {
    BugKind kind = BugKind::NullDeref;
    Span span;
    std::string message;
};

struct CheckReport {
    std::vector<BugFinding> findings;
    uint32_t nullDerefCount = 0;
    uint32_t outOfBoundsCount = 0;
    uint32_t nanPoisonCount = 0;

    bool empty() const { return findings.empty(); }
    std::string render(const SourceSet& sources) const;
    std::string render(const SourceBuffer& buffer) const;
};

// Runs path-sensitive symbolic abstract interpretation over `astMod` (using
// host API nullability contracts, inferred types, and lowered IL when provided)
// to find potential Null/Undefined Dereferences, Out-of-Bounds Array/TypedArray
// accesses, and NaN Poisoning hazards.
CheckReport checkModule(const ast::Module& astMod,
                        const types::InferenceResult* inferred = nullptr,
                        const il::Module* ilMod = nullptr,
                        DiagnosticSink* diags = nullptr,
                        bool strictDomNullability = true);

// Convenience entry point used by `bronze check <file>`: links the module graph
// from `sourcePath`, runs inference and static bug checking, prints findings to
// `outString` (or stdout/stderr), and returns 0 when clean or 1 when findings or
// load/parse errors occur.
int runCheckFile(const std::string& sourcePath,
                 std::string* outString = nullptr,
                 const std::vector<modules::ModuleRoot>& moduleRoots = {},
                 const std::string& importMapPath = {});

}  // namespace bronze::check
