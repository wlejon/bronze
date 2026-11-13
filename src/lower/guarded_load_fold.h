#pragma once

#include "il/il.h"

namespace bronze::lower {

// REPEATED GUARDED LOADS: a shape-guarded read (il.h `shapeGuarded`, with no
// layout fact) is a load of one slot of one object, and a second one of the
// same receiver and slot, with nothing between them that can store a slot or
// run code, is the first.
//
// What makes that worth a pass is the guarded regions' fast copies after
// inlining (guard_region.h, "SHAPE GUARDS" and "RE-ENTRY"): there a static
// site's miss leaves for the slow copy instead of rejoining, so the code
// between two inlined accessors' reads of `this._position` is tests and loads,
// where each miss used to be a call that could have stored the field. With the
// reads one value, the guards on it are guards of one SSA value, which the
// backend's shape memo answers with a compare each (il_shape_memo.cpp).
//
// The facts carried along a path, each with the value that answers it:
//
//   - a slot's content: (receiver, slot) after a shape-guarded read of it, or
//     after a shape-guarded store, whose value it then is. A shape-guarded
//     store forgets every other content at its slot number: two receivers may
//     be one object. Everything that can collect forgets all of them, since
//     that is everything that may call, allocate or store through a miss
//     (il::canCollect names what cannot, and nothing it names stores a named
//     slot).
//   - a shape test that held (`is.shape`, on the edge it holds on): a later
//     test of the same receiver that asks the same cell, or a family range
//     holding the earlier one's, is true. Forgotten where contents are, less
//     the stores: a shape-guarded store moves no shape.
//   - a value known a Number (an `is.number` that held, or a `box.f64`), and
//     the double of a value (`unbox.f64 raw`): facts about a value, never
//     forgotten.
//
// At a join a fact survives when every incoming edge carries it with the same
// answer; a loop header, a handler and a block reached before one of its
// predecessors start with none. A test folded to true takes its branch's other
// edge away, and the blocks that leaves unreachable (the trampoline of a shape
// point asked twice) go with it.
//
// Runs after the last guarded-region pass, on every function. True when it
// changed anything.
bool foldGuardedLoads(il::Module& module);

}  // namespace bronze::lower
