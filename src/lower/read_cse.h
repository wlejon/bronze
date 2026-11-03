#pragma once

#include "il/il.h"

namespace bronze::lower {

// REPEATED READS: a property or environment read that an earlier read of the
// same receiver and key dominates, with nothing between them that can write a
// property or a binding or run code, is that earlier read.
//
// `a.groupOrder !== b.groupOrder ? a.groupOrder - b.groupOrder : ...` reads
// each side twice, and the reads between are reads: no store, no call, no
// coercion (a strict compare runs nothing). A [[Get]] repeated over nothing
// returns what it returned unless something answers it with code, and three
// things can: an accessor, a Proxy's `get` trap, and a host object's native
// getter. The first is excluded by key: a key some getter may be installed
// under (read_cse.cpp `getterKeys`) is never folded, and a module that can
// install one under a key it does not spell folds nothing. The
// second is taken as the loop hoist takes it (loop_prop_analysis.cpp
// `canMutateProperty`): a plain-key read runs no code, and only a function
// that builds a Proxy or reflects itself is left alone. The third answers a
// host object's own state, which no code between the two reads ran to change.
//
// An `env.get.tdz` of a binding read before over nothing is the same read
// too: the earlier one already threw if the binding was uninitialized, and
// nothing between could have assigned it.
//
// Before the guarded regions, so the reads they guard are the ones left.
bool eliminateRepeatedReads(il::Module& module);

}  // namespace bronze::lower
