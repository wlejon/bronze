#pragma once

#include "il/il.h"

namespace bronze::lower {

// LOOP-INVARIANT TRUTH TESTS: `unbox.bool %v` (ToBoolean, which runs no code
// and cannot throw) inside a loop, of a value defined outside it, moves to
// the loop's preheader.
//
// eventemitter3's `removeListener(event, fn, context, once)` walks the
// listeners testing `once` and `context` on every one: each test is a tag
// dispatch on a boxed parameter (a dozen instructions for an object, which
// is what `context` is), and the answer is the same for the whole walk.
//
// Before the guarded regions and the typed loop copies, so every copy of the
// loop reads the one answer.
bool hoistLoopInvariantTruths(il::Module& module);

}  // namespace bronze::lower
