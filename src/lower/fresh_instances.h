#pragma once

#include <cstdint>

#include "il/il.h"

namespace bronze::lower {

// INSTANCES NOBODY HOLDS: `new V(x, y, z).add(a).scale(s)` makes three
// objects and keeps none. Once the constructor and the methods are inlined
// into a guarded region's fast copy (inline_guarded.h, guard_region.h "IC
// EXITS"), an instance has one definition, `create.this`, and its uses are
// the constructor's stores, reads of the keys those stored, the guards of the
// bodies inlined on it, and the exits that carry it to the slow copy. Then
// the fast copy never needs the object:
//
//   - the constructor's guard also asks that its stores are a transition
//     run's plain adds on the instance it would make now (`closure.is ...,
//     run @R`, il_op.h), so skipping them skips no setter and no refusal;
//   - a read of a key the run stored is the value stored;
//   - a method guard on it asks the shape the run ends at
//     (`run.method.is`), which is the shape the object would have;
//   - a shape test on it guarded only those reads, and is true: what it
//     also said, that a slot holds a Number, is asked of the values instead
//     (a stored value that is no Number's box has no unchecked unbox among
//     its readers, or the instance stays);
//   - an exit that carries it makes it first: the `create.this` and the
//     stores as they were written, in the exit's trampoline.
//
// The object exists on an exit exactly as it would have at its `new`,
// because nothing between the two can run code or collect: every instruction
// the instance is live across is a load, a test or arithmetic (il::canCollect
// says no), so the constructor's prototype and the chain the stores walk are
// what they were. An instance live across anything else keeps its allocation.
//
// R is the run of the SLOW copy's constructor, found by the stores' shared
// inline-cache site: the slow copy is where a miss lands and so where the
// run is published, and a fast copy asking its own run's cells would ask
// cells nothing fills.
//
// Runs after `markTransitionRuns`, on the final control flow of every
// function with a guarded region. Returns the instances removed.
uint32_t sinkFreshInstances(il::Module& module);

// What neither runs code nor collects: everything il::canCollect clears, a
// shape-guarded site (a bare load, or the bare store of a Number, behind its
// `is.shape`), and an environment slot's load.
bool runsNoCode(const il::Instruction& inst);

}  // namespace bronze::lower
