#pragma once

#include <optional>

#include "ast/ast.h"
#include "il/il.h"
#include "support/diagnostics.h"
#include "types/result.h"

namespace bronze::lower {

// The AST -> IL pass. `inference` is the side table inference produces; it is
// nullable, and passing null reproduces the pre-inference calling convention
// exactly — every value `Dynamic`, every user function reached the uniform
// dynamic way. That is the `--no-infer` seam and the bisection tool for any
// miscompile inference is suspected of causing.
//
// It reproduces the pre-inference *convention*, not the pre-inference bugs: an
// unproven answer is the dynamic path in BOTH modes, so the seam compares two
// correct programs. Two such sites: `s += "b"` on a local, and every TS
// annotation — which types nothing without a proof, so with no inference result
// it types nothing at all. The warnings that normally name a discarded
// annotation are silent here: with nothing provable they would fire on every
// annotation in the file and report only which switch is on. Unreadable
// annotation text is still the hard error it is in the other mode — this is a
// bisection seam, not a laxer compiler.
std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags,
                                      const types::InferenceResult* inference = nullptr);

}  // namespace bronze::lower
