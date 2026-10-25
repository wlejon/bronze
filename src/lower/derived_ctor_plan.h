#pragma once

// Where a derived constructor has to test whether `super()` has run.
//
// The receiver of a derived class's constructor does not exist until the base
// constructor returns it (9.1.1.3 BindThisValue): reading `this`, writing a
// property of it, `super.x`, an arrow that captures it, returning without an
// object — each is a ReferenceError until `super()` has returned, and a second
// `super()` is one after. bronze allocates the instance before the constructor
// runs, so nothing would throw on its own; the language's state has to be
// tracked.
//
// Tracked at run time only where it cannot be decided here. The common
// constructor is straight-line — `super(...)` as a statement, then the body —
// and every site in it is decided: the receiver is bound at every use after the
// call and none comes before it. Those sites get no test and the constructor
// gets no state, so its code is exactly what it was before any of this existed.
// A site the walk cannot decide — `this` after a `super()` in only one arm of
// an `if`, anything in a loop that calls `super()`, a `super()` inside an
// arrow that could run at any time — is listed, and lowering tests a flag in
// the constructor's environment there.

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"

namespace bronze::lower {

struct DerivedCtorPlan {
    // The sites lowering tests: `ThisExpr`, `SuperMember` and `ReturnStmt`
    // nodes whose receiver may be unbound, and `SuperCall` nodes that may not
    // be the first call. Keyed by node address, so a node absent from the set
    // — a field initializer the class desugar synthesized after the call, a
    // nested function's own `this` — is one that needs no test.
    std::unordered_set<const void*> checked;
    // Falling off the end of the body returns the receiver, and may do so
    // before any `super()` ran.
    bool endChecked = false;
    // How many `super()` calls the body holds, arrows included, and whether
    // one of them is a statement of the body's own list: what decides where
    // the field initializers go (lower_class.cpp).
    size_t superCalls = 0;
    bool topLevelSuperStatement = false;

    bool needsState() const { return endChecked || !checked.empty(); }
    bool isChecked(const void* node) const { return checked.count(node) != 0; }
};

DerivedCtorPlan planDerivedCtor(const std::vector<ast::Param>& params,
                                const std::vector<ast::StmtPtr>& body);

}  // namespace bronze::lower
