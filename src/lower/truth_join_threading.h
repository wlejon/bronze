#pragma once

#include "il/il.h"

namespace bronze::lower {

// TRUTH JOINS: a block that only asks ToBoolean of its one parameter and
// branches on the answer, entered along an edge whose argument's truthiness
// is already known there, is skipped along that edge.
//
// `if (a && r.x === true) ...; else if (...)` lowers `&&` as a value: a join
// whose parameter is `a` from the edge that found it falsy and the boxed
// Boolean of `r.x === true` from the other, then `unbox.bool` of the join's
// parameter and a branch. Every arm of an `else if` chain pays that boxing,
// the join, and a truthiness test of a value whose answer each edge had in
// hand. Along an edge
//
//   - `jump J(box.bool x)`, the answer is `x`: the jump becomes `br x` to
//     J's two targets;
//   - `br c, J(a), ...` or `br c, ..., J(a)`, where `a` is `box.bool c` or
//     `c` is `unbox.bool a` (ToBoolean of it), the answer is the edge's own
//     side of `c`: the edge goes straight to J's target for it.
//
// A block that only forwards its parameters with a `jump` is skipped the
// same way, its arguments substituted. Only blocks of one copy (CopyClass,
// copy region) are threaded into each other, only toward later blocks (so it
// ends), a join no edge enters any more is dropped, and a
// function whose rewrite would leave a use its definition does not dominate
// is left as it was.
//
// Last of the final passes: it changes control flow only, names no site,
// and the joins it removes are what keeps the backend's shape memo from
// flowing down the chain (il2mir il_shape_memo.cpp).
bool threadTruthJoins(il::Module& module);

}  // namespace bronze::lower
