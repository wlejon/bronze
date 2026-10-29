#pragma once

#include "il/il.h"

namespace bronze::lower {

// TRANSITION RUNS: a constructor's `this.a = a; this.b = []; this.c = 0; ...`
// adds one key per write, and lowered write by write each one asks the
// receiver's shape, misses every cache that names a shape it does not have yet,
// and takes a shape transition of its own. A run is such a stretch, marked on
// its first write (il.h `transitionRunCell`) so the backend can lower it as one
// guard and one shape store: the receiver still at the shape the run started
// from last time, the prototype chain unchanged since then, and every write
// after it a store at a constant slot (codegen-brass il_transition_run.cpp).
//
// What may stand between two writes of a run is the reason the backend can
// move the shape store to the front: nothing that runs user code, throws,
// or reads the heap — constants, number arithmetic, boxing, a ToNumber of a
// value from before the run (which the guard proves a Number), and the
// allocation of an empty literal or array. So no code can observe the
// receiver between its first and last write, and installing the final shape
// first is the same program.
//
// Marked where the slots are known at compile time: every write a static
// slot (il.h `staticSlot`) and the slots consecutive, or the receiver a
// `create.object` of this block that nothing has touched before the run, whose
// keys then land at 0, 1, 2, ... The runtime checks both claims against the
// shape it builds before it publishes anything, so a wrong one is a guard that
// never matches. Writes to `this` are marked only in a function that can be a
// constructor, and only to its own `this`: elsewhere a run of writes is a
// setter's `this.x = x` (inlined or not) over keys
// that already exist, and its guard would be one more miss in front of the
// static run that answers it.
//
// Runs last, after every pass that copies instructions: a copy of a marked
// write would share its cells with the original.
void markTransitionRuns(il::Module& module);

}  // namespace bronze::lower
