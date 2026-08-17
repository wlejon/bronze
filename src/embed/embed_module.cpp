// The loadable-module seam: what a host needs when its modules arrive as
// libraries it opened rather than objects it linked.
//
// Its own translation unit for the reason embed_run.cpp is one, inverted.
// That file is quarantined because it NAMES `bronze_main`, so anything linking
// it must define that symbol. Nothing here names a compiled symbol at all —
// the entry arrives as a pointer — which is exactly what makes these three
// usable from a host that has no linked module and may hold several loaded
// ones.

#include <cstdint>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/microtask.h"
#include "runtime/rt_state.h"

namespace bronze::embed {

uint32_t abiFingerprint() { return BRONZE_ABI_FINGERPRINT; }

void runEntry(ModuleEntry entry) {
    if (entry == nullptr) return;
    // The root frame runMain opens, for the same reason: Rooted<> handles
    // inside runtime helpers register here, and a host calling in from its own
    // frame loop has no bronze frame on the stack. Generated code links its
    // own contiguous slot frames onto bronze_gc_frame_top separately.
    //
    // NOT the ABI check: the fingerprint the loader must compare is the
    // MODULE's stamp, and a module that arrived through dlopen carries it as a
    // separate symbol the loader resolves itself. Doing it here would mean
    // guessing the stamp's name from the entry's, which is the loader's fact,
    // not this function's.
    bronze::ShadowStackFrame root_frame;
    entry();
    // A module whose top level queued a job has not finished running until the
    // job has — runMain's checkpoint, per module.
    runtime::rtDrainMicrotasks();
}

// The unload seam is one call each way because the mechanism lives with the
// spans it removes (rt_state.cpp): what belongs HERE is the contract, and
// embed.h carries it — the bracket discipline, the leak-the-image rule, and
// what "unload" does and does not free.
ModuleHandle beginModuleLoad() { return runtime::rtBeginModuleEpoch(); }
void unloadModule(ModuleHandle module) { runtime::rtDropModuleEpoch(module); }

void collectGarbage() { runtime::rtHeap().collect(); }

}  // namespace bronze::embed
