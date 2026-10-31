// The marking cycle on the host surface (embed_gc.h): brass's steps on the
// calling thread's heap. Thin on purpose. Everything bronze adds to a full
// collection rides on brass's own seams, which a cycle runs exactly as a
// back-to-back full collection does: the runtime's root sources are visited
// at the start and again at the finish (with a full collection's tracer
// both times), and its post-collection hooks (the dynamic-module sweep, the
// user prototype shapes, the weak tables, the buffer-view table) run once,
// at the finish, with collecting_full() true. Its finalizers run after the
// finish, as after collect().

#include "embed/embed_gc.h"

#include "runtime/heap.h"
#include "runtime/rt_state.h"

namespace bronze::embed {

bool startGcCycle() { return runtime::rtHeap().gc().start_marking(); }

bool gcMarkSlice(uint64_t budgetNs) { return runtime::rtHeap().gc().mark_slice(budgetNs); }

void finishGcCycle() { runtime::rtHeap().gc().finish_marking(); }

bool gcSweepSlice(uint64_t budgetNs) { return runtime::rtHeap().gc().sweep_slice(budgetNs); }

unsigned startGcHelperThreads() { return runtime::rtHeap().gc().start_helper_threads(); }

}  // namespace bronze::embed
