#pragma once

#include "ast/ast.h"

namespace bronze::lower {

// Can a BigInt reach an arithmetic operator anywhere in this program?
//
// The question exists because of one line in `lowerBinary`: `*`, `-`, `/` and
// `%` over a boxed operand have to produce a boxed result, because 13.15.3
// over two BigInts is a different algorithm returning a HEAP value. That
// result type is what puts the value in a GC root slot, and a rooted value is
// spilled and reloaded around every instruction — so an unremarkable
// `a * b + c * d` over dynamic operands cannot keep a single intermediate in a
// register. With no BigInt in the program, ToNumeric IS ToNumber, and the
// whole family becomes `unbox.f64` operands with an f64 result.
//
// The scan is deliberately syntactic and deliberately over-broad: it answers
// "no" only when the program mentions nothing that could name or build a
// BigInt to compute with: the literal suffix, the `BigInt` constructor and a
// host's `bigint` member, in any position. The BigInt-typed views and
// `DataView`'s accessor pairs are left out on purpose (bigint_reach.cpp).
//
// Host globals are checked by NAME too, from the manifest: a host that
// registers `BigInt` is a host whose values can carry one.
bool bigIntMayReach(const ast::Module& module,
                    const std::vector<std::string>& hostGlobals);

}  // namespace bronze::lower
