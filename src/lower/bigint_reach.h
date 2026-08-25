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
// BigInt. Every route in the language is a name — the literal suffix, the
// `BigInt` constructor, the two BigInt-typed views, and `DataView`'s two
// accessor pairs — so a walk that refuses on any of those spellings, in any
// position, cannot miss one. Being wrong in the "yes" direction costs the
// optimization; being wrong in the "no" direction is a miscompile, which is
// why an unrecognised spelling is never assumed harmless.
//
// Host globals are checked by NAME too, from the manifest: a host that
// registers `BigInt` or a BigInt view is a host whose values can carry one.
bool bigIntMayReach(const ast::Module& module,
                    const std::vector<std::string>& hostGlobals);

}  // namespace bronze::lower
