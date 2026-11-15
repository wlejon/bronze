#pragma once

#include <cstdint>

#include "il/il.h"

namespace bronze::lower {

// QUIET LOOPS: a loop of a guarded region's fast copy in which nothing runs
// code or collects (`runsNoCode`, fresh_instances.h) is tests, loads, stores
// of Numbers and arithmetic. Once the calls in it are inlined bodies and the
// instances they made are gone (inline_guarded.h, fresh_instances.h) that is
// the whole of `s += new V(i, 1, 2).add(a).scale(0.5).dot(a)`, and most of it
// asks the same questions on every trip: is `V` still the closure it was, is
// `a` still the shape whose slots these are, what is `a.x`. No trip can change
// an answer, because changing one takes a call or an allocation, so each is
// asked once, in front of the loop:
//
//   - a test of values the loop does not define (`is.shape`, `closure.is`,
//     `method.ic.is`, `run.method.is`, `is.number`) and an environment slot's
//     load, when the loop stores to no environment, move as they are. The
//     branch on a test stays where it was and reads the one answer; a test
//     asked twice is asked once.
//   - a shape-guarded load from such a receiver, and the unboxing of what it
//     loaded, are only loads behind their `is.shape`: they move to an arm in
//     front of the loop that the test's answer selects, and the loop reads a
//     parameter of the join behind it, which on the other arm is a value the
//     loop never looks at (its branch on the same answer leaves first). A
//     slot some store of the loop may write stays a load in the loop.
//
// Then a `box.f64` only the exits read is made in each exit's trampoline
// instead: the fast copy's doubles are boxed for the slow copy and nothing
// else.
//
// Runs last, on the control flow the final passes left, and not in a build a
// profile is read against or recorded from: it moves sites, and a profile
// numbers them in the order the final IL has them. Returns the instructions
// moved.
uint32_t hoistQuietLoops(il::Module& module);

}  // namespace bronze::lower
