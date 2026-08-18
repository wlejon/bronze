#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "support/diagnostics.h"
#include "types/result.h"

namespace bronze::types {

// Runs inference over a parsed module and returns the side table lowering
// reads. The AST is not mutated.
//
// Failing to prove something is never a failure: it is a fallback to
// `Dynamic`, which is the designed answer. `std::nullopt` means an internal
// impossibility was hit — an AST node kind the analysis does not know, or a
// fixpoint that did not converge — and a diagnostic naming it is in `diags`.
//
// `hostGlobals` is the `--host-globals` manifest, when the caller has one: a
// host that registers its own `Math`, `globalThis`, `Float64Array` or
// `Float32Array` replaces the builtin the builtin-identity proofs name, so
// those proofs are withheld. Null means no manifest.
std::optional<InferenceResult> inferModule(const ast::Module& module, DiagnosticSink& diags,
                                           const std::vector<std::string>* hostGlobals = nullptr);

}  // namespace bronze::types
