#pragma once

#include <set>
#include <string>

#include "ast/ast.h"

namespace bronze::types {

// Decision 5's escape test: the set of names that appear anywhere in the
// module in a position other than "callee of a call".
//
// A module-level function whose name is not in this set has no unknown
// callers, so joining the types seen at its call sites is a proof about
// every call it will ever receive. One escaping reference — read as a value,
// passed as an argument, stored on an object, closed over — and the function
// keeps the uniform dynamic convention. The test is deliberately blunt; the
// point is that it is obviously sound, not that it is tight.
//
// Two positions are counted as escapes that a purely syntactic reading might
// not expect, both for soundness:
//
//   - `new F()`: a constructor call binds `this` and yields an object, so
//     the specialized direct-call convention does not describe it.
//   - any *binding* of the name anywhere below the module top level (a
//     parameter, a `let`, a nested function declaration). Such a binding
//     shadows the module function, so a call through that name is not a call
//     to the module function at all.
std::set<std::string> escapingNames(const ast::Module& module);

}  // namespace bronze::types
