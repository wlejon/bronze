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

// `var` declarations written directly at the top level of `stmts` (not inside blocks).
std::vector<std::string> getTopLevelVarDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getTopLevelVarDeclarations(const std::vector<const Stmt*>& stmts);

// The LEXICAL half of `getScopeDeclarations`: the `let`, `const` and `class`
// names this statement list declares directly, in source order. A hoisted
// `function` declaration is deliberately absent — 14.3.1 leaves a lexical
// binding uninitialized until its declaration is evaluated, and 8.6.2
// instantiates a function declaration for its whole scope, so a function name
// is never in a temporal dead zone and must never be given one.
std::vector<std::string> getLexicalDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getLexicalDeclarations(const std::vector<const Stmt*>& stmts);

// The `const` names this statement list declares directly, in source order.
//
// A separate question from `getLexicalDeclarations` because 14.3.1.1 creates a
// `const` binding with `CreateImmutableBinding(name, true)`, and that `true` is
// the S that 9.1.1.1.5 step 7 tests: an assignment to one is a TypeError
// whatever the strictness of the code doing the assigning. The other immutable
// binding in the language — a named function expression's own name (15.2.5) —
// is created with `false` and IS strictness-dependent, which is why the two
// cannot share a flag.
std::vector<std::string> getConstDeclarations(const std::vector<StmtPtr>& stmts);
std::vector<std::string> getConstDeclarations(const std::vector<const Stmt*>& stmts);

// The lexical names of `stmts` whose initializer runs before ANY user code can
// run in this scope — so that no read of them, from anywhere including a
// closure, can happen while they are still in their dead zone.
//
// The argument is about who can be RUNNING, not about where the read is
// written. A closure over a `let` is entered only by someone calling it, and
// calling it is user code; so if every statement from the top of the scope
// down to a declaration is one that runs no user code — another such
// declaration, or a hoisted `function` — then nothing can have called anything
// yet, and every read of that binding in the whole program is after its
// initializer. The dead zone is real but unreachable, and 9.1.1.1.6's check is
// then a compare that can only answer one way.
//
// Stage E4 sharpened "runs no user code" into "runs no user code THAT COULD
// READ ONE OF THESE BINDINGS", which is the question the paragraph above is
// actually asking. `makeLCG(12345)` runs a great deal of user code and not one
// instruction of it can see this scope's record, because a closure over that
// record has to have been CREATED here and its value has to have got out.
//
// So the scan tracks two things instead of stopping at the first call:
//
//   - the DANGEROUS names: the hoisted `function` declarations of this list
//     (8.6.2 instantiates all of them, closed over this record, before
//     statement one) plus any binding the prefix has already filled with a
//     function expression. A statement that mentions one of those is a moment
//     at which such a closure can be entered or handed out, and the scan stops.
//   - the names not yet declared. A statement that mentions one of those reads
//     it in its own dead zone, which is the ReferenceError the check is for.
//
// A statement also stops the scan if it CREATES a function or class value
// anywhere inside itself other than as the whole of a declaration's
// initializer: an IIFE, a callback argument and an object literal's method are
// each a closure over this record that the statement itself can enter.
//
// `params` is the enclosing function's parameter list, or null for a list that
// has none (the module top level, a block). A parameter DEFAULT is code of this
// scope, so a default that builds a function puts a closure over this record in
// a parameter — a name the scan would otherwise treat as harmless — and the
// whole widening is refused for that scope rather than modelled.
//
// It is still a licence to drop a check, so every clause is a refusal in the
// safe direction and a name it is unsure of is a name it leaves alone.
// `tests/oracle/cases/dead_zone_reachability.js` pins the boundary from the
// other side.
std::vector<std::string> getDefinitelyAssignedLexicalNames(
    const std::vector<StmtPtr>& stmts, const std::vector<Param>* params = nullptr);
std::vector<std::string> getDefinitelyAssignedLexicalNames(
    const std::vector<const Stmt*>& stmts, const std::vector<Param>* params = nullptr);

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
bool usesThis(const std::vector<Param>& params, const std::vector<StmtPtr>& stmts);
bool usesThis(const std::vector<Param>& params, const std::vector<const Stmt*>& stmts);
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

// --- what a SUSPENSION needs (queries_yield.cpp) ------------------------

// Is there a suspension — a `yield` or an `await` — anywhere under this node
// that belongs to THIS body? Stops at every nested function boundary: a
// `yield` written inside a nested generator is that generator's suspension,
// an `await` inside a nested async function is that function's, and a body
// that is neither cannot contain either at all — both words are ordinary
// identifiers there. `await` answers here because it IS a `YieldExpr`
// (ast.h says why), so every consumer of "where can this body re-enter"
// covers both forms without a second walk.
bool containsYield(const Node& node);
bool containsYield(const std::vector<StmtPtr>& stmts);

// WHICH suspension forms are under there. Two consumers, and neither can use
// the boolean above: the lifter has to NAME the construct it refuses, and a
// message that says `yield` about a `yield*` sends the reader looking for a
// restriction that is not the one they hit; and lowering gives a generator's
// frame a slot for the delegated iteration only when the body has a delegation
// to hold in it. `Await` is a bit of the same set because the parser keeps
// generators and async functions apart, so a body only ever holds Await bits
// or yield bits — never both — and the form name below can stay one phrase.
enum class YieldForms : uint8_t { None = 0, Plain = 1, Delegating = 2, Both = 3, Await = 4 };
inline YieldForms operator|(YieldForms a, YieldForms b) {
    return static_cast<YieldForms>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool hasDelegating(YieldForms f) {
    return (static_cast<uint8_t>(f) & static_cast<uint8_t>(YieldForms::Delegating)) != 0;
}
inline bool hasAwait(YieldForms f) {
    return (static_cast<uint8_t>(f) & static_cast<uint8_t>(YieldForms::Await)) != 0;
}
YieldForms yieldFormsIn(const Node& node);
YieldForms yieldFormsIn(const std::vector<StmtPtr>& stmts);
YieldForms yieldFormsIn(const std::vector<const Stmt*>& stmts);
// The noun phrase a refusal names those forms by: "a `yield`", "a `yield*`",
// "an `await`", or "a `yield` or a `yield*`" where the refused position holds
// both yield forms. Await never mixes with the others in one body (the parser
// refuses async generators), so no phrase has to cover that.
const char* yieldFormName(YieldForms forms);

// Every binding a GENERATOR's frame must hold: each name declared anywhere under
// `stmts`, at any block depth, nested functions excluded.
//
// The third reason a binding cannot live in SSA, and a sibling of the other two:
// `getCapturedNames` says a closure can reach it, `getTryAssignedNames` says a
// handler is entered from a point no join can enumerate, and this one says a
// SUSPENSION is such a point. Lowering re-enters a generator by jumping from the
// resume function's entry block to the block after a `yield`, and that edge
// defines no SSA value at all, so everything the resumed code reads has to come
// out of the frame's environment record.
//
// Deliberately NOT a liveness analysis. "Which names cross a yield" is a
// question about a control-flow graph that does not exist when this is asked,
// and the honest over-approximation — the whole frame — costs a heap slot per
// local in a generator and can never lose a value. A narrower answer that got
// one binding wrong would not be a wrong answer, it would be a read of an SSA
// value the entry edge never defined: a wrong PROGRAM.
std::unordered_set<std::string> getGeneratorFrameNames(const std::vector<StmtPtr>& stmts);
std::unordered_set<std::string> getGeneratorFrameNames(const std::vector<const Stmt*>& stmts);

// How many `for-of`/`for-in` ITERATION RECORDS a machine body can have in
// flight at once — the deepest nest of such loops whose body holds a
// suspension, nested functions excluded.
//
// The record is the one thing a loop of that kind carries that is not a
// binding: `getGeneratorFrameNames` above cannot name it, because the source
// never did. It still has to be in the frame for the same reason every binding
// is, so the frame reserves this many anonymous slots and lowering hands out
// the one at its current nesting depth. A DEPTH rather than a count because
// two sibling loops are never stepping at once and can share a slot; only
// nesting makes two records live together.
uint32_t maxSuspendingIterationDepth(const std::vector<StmtPtr>& stmts);
uint32_t maxSuspendingIterationDepth(const std::vector<const Stmt*>& stmts);

}  // namespace bronze::ast
