#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"

namespace bronze::ast {

// Pure AST queries about scopes. They live here rather than in a consumer
// because more than one stage has to agree on the answers: lowering decides
// where a variable lives from them, and inference decides what it may
// believe about that variable from the same answers. Two hand-maintained
// copies of these rules would eventually disagree, and a disagreement is a
// silent miscompile — inference holding a flow-sensitive fact about a
// variable that lowering has put in an environment.
//
// Iteration order of the returned `unordered_set` must never reach an output
// path; sort at the boundary if it would.

// Every name referenced inside a function nested anywhere within `stmts`.
// A variable in this set must live in an environment record rather than in
// SSA, because a closure may read or write it after the declaring scope's
// SSA values are gone (docs/0007 decision 1).
//
// For the same reason, inference must treat such a variable as ONE cell
// joined across every write in the function rather than as a per-program-
// point fact: a closure can write it at any time, so flow sensitivity on it
// would be unsound (docs/0010 decision 3).
//
// Deliberately an over-approximation: it includes the nested functions'
// own locals and parameters, so an unrelated same-named variable in the
// enclosing scope is env-backed too. That costs a little speed and no
// correctness; narrowing it is escape analysis's job (docs/0004).
std::unordered_set<std::string> getCapturedNames(const std::vector<StmtPtr>& stmts);
std::unordered_set<std::string> getCapturedNames(const std::vector<const Stmt*>& stmts);

// Names declared directly by `stmts` — let/const and function declarations
// in this statement list only, NOT inside nested blocks or functions — in
// source order. This is the scope's own contribution to an environment.
std::vector<std::string> getScopeDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getScopeDeclarations(const std::vector<const Stmt*>& stmts);

// `var` declarations anywhere under `stmts` except inside nested functions,
// in source order: they are function-scoped wherever they are written.
std::vector<std::string> getHoistedVarDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getHoistedVarDeclarations(const std::vector<const Stmt*>& stmts);

// Does this function body mention `this`? Deliberately does NOT descend
// into nested functions: each one binds its own receiver, so an inner
// `this` says nothing about the outer function (docs/0008 decision 3).
// Decides whether the function gets the synthetic `__this` parameter,
// which is why it is a body property rather than a scope one.
bool usesThis(const std::vector<StmtPtr>& stmts);
bool usesThis(const std::vector<const Stmt*>& stmts);

}  // namespace bronze::ast
