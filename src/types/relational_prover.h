#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/ast.h"
#include "types/env.h"
#include "types/host_contracts.h"
#include "types/type.h"

namespace bronze::types {

using TypeLookup = std::function<Type(const std::string&)>;

// Path-sensitive type refinements extracted from a branch or ternary condition.
// `whenTrue` holds variable types proven on the truthy edge; `whenFalse` holds
// variable types proven on the falsy edge.
struct ConditionRefinements {
    std::unordered_map<std::string, Type> whenTrue;
    std::unordered_map<std::string, Type> whenFalse;
};

// Extracts path-sensitive type refinements from `cond` (`typeof x === "number"`,
// `Number.isFinite(x)`, `Number.isInteger(x)`, `Array.isArray(x)`, `x === 0`,
// `x !== undefined`, `x != null`, `!cond`, `a && b`, `a || b`, etc.).
ConditionRefinements extractConditionRefinements(
    const ast::Expr* cond, const TypeLookup& lookupType,
    const UserBindingPredicate& isUserBound = nullptr, bool mathPristine = true);

// Applies `refinements` to `env` for non-captured local bindings. Returns the
// original types of any refined variables so they can be restored if needed.
std::vector<std::pair<std::string, Type>> applyConditionRefinements(
    Env& env, const std::unordered_map<std::string, Type>& refinements,
    const std::function<bool(const std::string&)>& isCell);

// True when `stmt` or `stmts` is guaranteed to exit abruptly (`return`,
// `throw`, `break`, `continue`, or an `if`/`else` where both branches exit).
// Used to propagate the negated condition refinement after an early-exit guard
// (`if (typeof x !== "number") return 0;`).
bool stmtDefinitelyTerminates(const ast::Stmt& stmt);
bool stmtListDefinitelyTerminates(const std::vector<ast::StmtPtr>& stmts);

// Precision result typing for `lhs ?? rhs` and `lhs || rhs` (including
// default-value idioms like `(x ?? 0)` and `(x || 0)` on `Undefined`, `Null`,
// or `DynamicNumOrUndef` parameters).
Type nullishCoalescingResult(Type lhs, Type rhs);
Type logicalOrResult(Type lhs, Type rhs);

}  // namespace bronze::types
