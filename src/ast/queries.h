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

// Every name referenced inside a function nested anywhere within `stmts`. A
// variable in this set must live in an environment record rather than in SSA,
// because a closure may read or write it after the declaring scope's SSA values
// are gone.
//
// For the same reason, inference must treat such a variable as ONE cell joined
// across every write in the function rather than as a per-program- point fact:
// a closure can write it at any time, so flow sensitivity on it would be
// unsound.
//
// Deliberately an over-approximation: it includes the nested functions' own
// locals and parameters, so an unrelated same-named variable in the enclosing
// scope is env-backed too. That costs a little speed and no correctness;
// narrowing it is escape analysis's job.
std::unordered_set<std::string> getCapturedNames(const std::vector<StmtPtr>& stmts);
std::unordered_set<std::string> getCapturedNames(const std::vector<const Stmt*>& stmts);

// Every name mentioned anywhere under `stmts`, INCLUDING inside nested
// functions to any depth. The question `getCapturedNames` asks from the
// declaring side, asked from the referencing side: "could this body, or
// anything written inside it, need to reach that name?".
//
// Lowering asks it of a top-level function declaration to decide whether the
// function must load the module environment record at entry. An
// over-approximation is the safe direction — it costs one load in a function
// that turns out not to need it, where an under-approximation would be an
// unresolved name.
std::unordered_set<std::string> getReferencedNames(const std::vector<StmtPtr>& stmts);

// The same question asked of a PARAMETER LIST. A default value and a pattern's
// computed key are code that runs in the function and appears nowhere in its
// body, so a caller that scans only the body misses them.
std::unordered_set<std::string> getParamReferencedNames(const std::vector<Param>& params);

// Names declared directly by `stmts` — let/const and function declarations
// in this statement list only, NOT inside nested blocks or functions — in
// source order. This is the scope's own contribution to an environment.
std::vector<std::string> getScopeDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getScopeDeclarations(const std::vector<const Stmt*>& stmts);

// `var` declarations anywhere under `stmts` except inside nested functions,
// in source order: they are function-scoped wherever they are written.
std::vector<std::string> getHoistedVarDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getHoistedVarDeclarations(const std::vector<const Stmt*>& stmts);

// The LEXICAL half of `getScopeDeclarations`: the `let`, `const` and `class`
// names this statement list declares directly, in source order. A hoisted
// `function` declaration is deliberately absent — 14.3.1 leaves a lexical
// binding uninitialized until its declaration is evaluated, and 8.6.2
// instantiates a function declaration for its whole scope, so a function name
// is never in a temporal dead zone and must never be given one.
std::vector<std::string> getLexicalDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getLexicalDeclarations(const std::vector<const Stmt*>& stmts);

// Which lexical bindings anywhere in this function (but not inside a nested
// one, which asks the question again for itself) can be READ while they are
// still uninitialized, so that the read has to be checked at run time rather
// than resolved to an SSA value.
//
// Two sources, and they are different in kind:
//
//   - a name mentioned in its own scope's statement list ABOVE its
//     declaration. Within one activation of a scope control runs forward
//     through the list, so a mention above the declaration is a mention while
//     the binding is uninitialized — every time.
//   - every lexical binding declared directly in a `switch` body. ECMA-262
//     14.12.2 makes the whole CaseBlock one scope with as many entry points as
//     it has clauses, so no position in it is safe.
//
// A binding a nested function reads is NOT here: `getCapturedNames` already
// puts it in an environment record, and the check follows the slot rather than
// the name. Over-approximating costs an environment slot and a compare;
// under-approximating is a read of an uninitialized binding answering with
// whatever the enclosing scope's same-named binding holds.
std::unordered_set<std::string> getTdzExposedNames(const std::vector<StmtPtr>& stmts);
std::unordered_set<std::string> getTdzExposedNames(const std::vector<const Stmt*>& stmts);

// Does a function nested anywhere inside this `for` statement — its head, its
// condition, its update or its body — reference `name` in a way that resolves
// to the LOOP's binding of it?
//
// The question ECMA-262 14.7.4 forces: a `let` loop binding is copied per
// iteration, so a closure that reaches it must capture that iteration's copy.
// The copy is observable through a closure and through nothing else, so this
// is what decides whether lowering builds one at all. A closure that binds
// `name` itself — a parameter called `i`, a `let i` of its own — never touches
// the loop's, and `for (let i…)` beside a callback taking `i` is ubiquitous,
// so the two must not be confused.
//
// Answers "yes" whenever it cannot tell: a false yes is an environment record
// per iteration that nothing reads, a false no is a closure silently sharing
// one binding across every iteration.
bool closureCapturesLoopBinding(const ForStmt& forStmt, const std::string& name);

// Does this function body mention `this`? Deliberately does NOT descend into
// nested functions: each one binds its own receiver, so an inner `this` says
// nothing about the outer function. Decides whether the function gets the
// synthetic `__this` parameter, which is why it is a body property rather than
// a scope one.
bool usesThis(const std::vector<StmtPtr>& stmts);
bool usesThis(const std::vector<const Stmt*>& stmts);

// Does this function need an `arguments` object, and may it have one?
//
// Two questions in one answer, because they have one consumer. It descends into
// ARROWS and no further — an arrow has no `arguments` of its own and sees the
// enclosing function's, exactly as it does for `this` — and it answers false
// outright when the name is BOUND by a parameter or by a declaration in the
// body, because then the binding is what `arguments` means and no object
// exists.
//
// Parameter defaults are scanned too: they are code that runs in the function
// and appears nowhere in its body.
bool usesArguments(const std::vector<Param>& params, const std::vector<StmtPtr>& body);

// Does this function body contain a `return <expr>;`? Like `usesThis`, it
// does NOT descend into nested functions: an inner `return` returns from
// that function.
//
// Lowering needs this BEFORE it lowers any body, because a function's IL
// return type is part of its calling convention and its callers are lowered
// first — a mutually recursive pair has each caller reading the other's
// return type while that other body is still unlowered. "Has a value
// return" is the one thing about the answer that a single syntactic look
// can settle, and `dynamic` is the sound type for the rest.
bool returnsAValue(const std::vector<StmtPtr>& stmts);

}  // namespace bronze::ast
