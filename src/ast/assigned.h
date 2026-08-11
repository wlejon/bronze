#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"

namespace bronze::ast {

// Every name written by an assignment, an update operator or a declaration
// anywhere under `node`, stopping at nested function and class boundaries:
// those write their own scopes. It is what sizes a block-argument SSA join
// (docs/0005) — a name left out here is a variable frozen at its pre-loop
// value on a back edge.
//
// In `ast` rather than in `lower` because two stages need the same answer:
// lowering sizes joins with it, and `getTryAssignedNames` below — which both
// lowering and inference read — is defined in terms of it (docs/0020
// decision 4).
std::unordered_set<std::string> getAssignedNames(const Node& node);
std::unordered_set<std::string> getAssignedNames(const std::vector<StmtPtr>& stmts);

// Every name assigned anywhere inside a `try` statement under `stmts`, nested
// functions excluded. The second half of "which bindings cannot live in SSA"
// (docs/0020 decision 4): a handler block is entered from an arbitrary point
// in the protected region, so nothing at IL-construction time knows what such
// a binding held there, and there is no block-argument list to write. Union it
// with `getCapturedNames` and lowering and inference have one answer to that
// question, which is the rule `queries.h` states.
//
// Over-approximates in the same direction and for the same reason as
// `getCapturedNames`: it is name-based, so a binding declared INSIDE the try
// that shares a name with one outside is promoted too. That costs a slot and a
// load; it can never lose a value.
std::unordered_set<std::string> getTryAssignedNames(const std::vector<StmtPtr>& stmts);
std::unordered_set<std::string> getTryAssignedNames(const std::vector<const Stmt*>& stmts);

}  // namespace bronze::ast
