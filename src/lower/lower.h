#pragma once

#include <optional>

#include "ast/ast.h"
#include "il/il.h"
#include "support/diagnostics.h"
#include "types/result.h"

namespace bronze::lower {

// The AST -> IL pass. `inference` is the side table docs/0010 decision 1
// produces; it is nullable, and passing null reproduces the pre-inference
// calling convention exactly — every value `Dynamic`, every user function
// reached the uniform dynamic way. That is the `--no-infer` seam of
// decision 8 and the bisection tool for any miscompile inference is
// suspected of causing.
//
// It reproduces the pre-inference *convention*, not the pre-inference
// bugs: where lowering used to specialize on something it had not proven,
// the unproven answer is now the dynamic path in both modes. `s += "b"`
// on a local is the one such site today (docs/0010 decision 3).
std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags,
                                      const types::InferenceResult* inference = nullptr);

}  // namespace bronze::lower
