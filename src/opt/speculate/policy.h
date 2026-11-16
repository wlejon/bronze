#pragma once

// Which functions the step may speculate, and which direct calls S may make.
// mayPlaceResumePoint (speculate.h) is defined here too.

#include <cstddef>

#include "il/il.h"

namespace bronze::opt::speculate {

// Whether `fn` may get a speculated copy at all: a definition lowering made
// (not external, not already speculated or speculated from), not the module's
// entry point (it runs once, and the backend names it `main`), not a
// coroutine body (entered only through `coro.start`'s frame, and no resume
// point may sit in one), with blocks.
bool functionMaySpeculate(const il::Function& fn);

// THE DIRECT-CALL CONVENTION S may use for a call it speculates on: whether
// `call @callee(env?, this?, args...)` with `argc` source arguments in hand
// is exactly what the uniform path would do with them.
//
// The uniform path enters through the callee's wrapper, which binds `__env`
// from the closure, `__this` from the call, builds `arguments` and a rest
// array where the callee has them, pads a short argument list with
// `undefined` up to `adaptArity()` and drops the rest, converts each argument
// to its parameter's type, and refuses a class constructor. A direct call does
// only what its operands say, so it is used only where nothing else is left
// to do:
//   - functionEntersDirectly (il.h): no new.target or super() reader, not a
//     generator or async function, not native, not the entry point;
//   - not a class constructor (calling one throws; a direct call would run it);
//   - no `arguments` object (it is built from the count the wrapper sees) and
//     no rest parameter (the leftover array is the wrapper's to build);
//   - every source parameter Dynamic and none pinned, so each argument passes
//     as it is, with no conversion and no barrier;
//   - at most 16 arguments (the fixed-operand forms' limit).
// Arguments past `callerParamCount()` are dropped: evaluated already, and no
// longer observable without `arguments` or a rest parameter, which is what the
// wrapper does with them. Missing ones are `undefined`, the wrapper's padding.
bool directCallFits(const il::Function& callee, size_t argc);

// The same convention for the constructor of a `new` (construct.cpp): a
// class constructor too (what makes calling one throw is the runtime's call
// path, never its body), but one that binds `this`, so the instance is
// a parameter of the call.
bool constructFits(const il::Function& callee, size_t argc);

}  // namespace bronze::opt::speculate
