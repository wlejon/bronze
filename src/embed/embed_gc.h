#pragma once

// A full collection in steps a host spreads over time it has to spare (the
// gap between frames), rather than one pause at whichever allocation found
// the old generation full. A companion of embed.h, carrying BRONZE_EMBED_API
// for the reason it does: the heap is the ONE runtime's, the calling thread's.
//
//   if (p.oldGrowth > p.fullThreshold / 2 && !p.marking && !p.sweeping)
//       startGcCycle();                       // a pause: the roots
//   ... each frame, while p.marking:
//       if (gcMarkSlice(budgetNs)) finishGcCycle();
//   ... each frame, while p.sweeping:
//       gcSweepSlice(budgetNs);
//
// (p is heapPressure(), which says where the heap is: embed.h.) The steps are
// brass's marking cycle and lazy sweep (brass docs/gc_contract.md 2.10 and
// 2.11); the program runs between them, allocating, storing and collecting
// its young generation as always. Each step is a collection as far as the
// GC contract above collectGarbage is concerned: Persistents survive, raw
// pointers from typedArrayInfo / arrayBufferInfo die. A cycle is counted as
// one full collection, at its finish, which is where everything a full
// collection settles is settled: WeakRefs and WeakMaps cleared, finalizers
// and FinalizationRegistry cleanups queued, dynamic eval / Function modules
// nothing reaches dropped, the runtime's weak tables swept.
//
// What a cycle keeps is what was live when it started plus what was made
// since (floating garbage is the next cycle's). The heap keeps its own
// triggers while a cycle is open: old-generation growth past its threshold
// finishes the cycle at that allocation, and collectGarbage finishes it and
// then runs a whole collection, so a host that stops slicing never makes the
// heap fail, only pause where it would have anyway.

#include <cstdint>

#include "embed/embed.h"

namespace bronze::embed {

// Opens a cycle: the roots are visited and what they name in the old
// generation marked grey, after the last cycle's sweep is completed if any
// is left (so sweep it out first with gcSweepSlice to keep this short).
// False, doing nothing, when a cycle is already open.
//
// None of the four may be called from inside a collection (a post-collection
// hook, a root source): that stops the process, as brass's steps do.
BRONZE_EMBED_API bool startGcCycle();

// Marks for about `budgetNs` (always some, however small the budget) and
// returns whether the grey work is all gone: then finishGcCycle has only
// what the program shades between now and then left to mark. A slice of at
// least 0.2 ms runs on the marker's threads. Calling it with no cycle open
// (heapPressure().marking false: never started, or already finished by the
// heap's own trigger or collectGarbage) stops the process.
BRONZE_EMBED_API bool gcMarkSlice(uint64_t budgetNs);

// Finishes the open cycle, a pause: the young generation evacuated, the roots
// visited again, the rest of the grey work drained, then weak references,
// finalizers and the runtime's per-collection sweeps settled as at the end
// of collectGarbage's collection. The old generation's sweep is left for
// gcSweepSlice (and the allocator). No cycle open stops the process.
BRONZE_EMBED_API void finishGcCycle();

// Sweeps the finished cycle's old generation for about `budgetNs`; true
// once nothing is left (at once when nothing was).
BRONZE_EMBED_API bool gcSweepSlice(uint64_t budgetNs);

// Creates the collector's parked helper threads now (brass
// Heap::start_helper_threads), the ones a parallel minor collection copies
// and a mark slice or full collection marks on, rather than in the pause of
// the first collection that wants them: some 0.1 ms a thread, 0.7 ms of an
// eight-thread first minor. For a host that knows a moment when that costs
// nothing, once (after its boot, before its first timed frame); a heap that
// never collects in parallel never needs them. Returns how many helpers
// exist. Not from inside a collection.
BRONZE_EMBED_API unsigned startGcHelperThreads();

}  // namespace bronze::embed
