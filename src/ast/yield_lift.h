#pragma once

#include <string>
#include <vector>

#include "ast/ast.h"
#include "ast/queries.h"
#include "support/diagnostics.h"

namespace bronze::ast {

// Rewrites a generator body so that every `yield` stands ALONE at a statement
// boundary — either as a whole `ExprStmt`, or as the entire initializer of a
// freshly declared binding — and never inside a larger expression.
//
// That is the precondition the state machine needs, and it is a precondition
// about SSA rather than about syntax. Lowering suspends a generator by RETURNING
// from the resume function and re-entering it at a block that the entry block
// jumps to; every IL value live at that point has to be reachable from the
// frame's environment record, because the entry edge defines none of them. A
// binding is: lowering puts every one of a generator's bindings in the record.
// An intermediate is not — `f(a, yield 1)` holds `a` in an SSA value across the
// suspension, and there is no name for lowering to spill it under. So the
// rewrite gives every such intermediate a name:
//
//     f(a, yield 1)   ->   let t0 = a; let t1 = yield 1; f(t0, t1)
//
// `a` is pinned into `t0` and not left in place because the resumption runs
// ARBITRARY code between the two: whatever `a` denotes may have changed, and
// ECMA-262 evaluates it before the yield.
//
// Two forms need a statement and not just a temporary, because an operand of
// theirs is evaluated only on some paths: `&&`/`||`/`??` and `?:` become an
// `if`. Everything else is a left-to-right list.
//
// Returns false with a diagnostic reported for the positions bronze refuses by
// name (see the messages in the implementation); the body is then meaningless
// and must not be lowered.
bool liftYields(std::vector<StmtPtr>& body, const std::string& tempPrefix,
                DiagnosticSink& diags);

}  // namespace bronze::ast
