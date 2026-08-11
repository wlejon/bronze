#pragma once

#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze {

// A getter or a setter, run with `this` bound to the RECEIVER of the
// property operation rather than to the object that holds the accessor
// (ECMA-262 10.1.8.1 / 10.1.9.2, docs/0019 decision 3). That is what makes
// an accessor defined once on a prototype a computed field on every
// instance, and reading it wrong is invisible until a second instance
// exists.
//
// Both ALLOCATE — a getter is user code — so the receiver arrives as a root
// and the caller must not hold a raw pointer to it across the call. This is
// the load that used to be a plain slot read (docs/0006 decision 4): the
// safepoints are the helper calls, and a property read is now one of them.

// `getter` undefined is a set-only accessor, whose read is `undefined` —
// OrdinaryGet's answer, not an error.
Value callGetter(Value getter, Rooted<Value>& receiver);

// `setter` undefined is a get-only accessor. ECMA-262 10.1.9.2 step 5.c
// returns false there, which the caller's non-strict Set discards: the write
// is a silent no-op (docs/0019 decision 6).
void callSetter(Value setter, Rooted<Value>& receiver, Rooted<Value>& value);

}  // namespace bronze
