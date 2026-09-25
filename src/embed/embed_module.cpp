// The loadable-module seam: what a host needs when its modules arrive as
// libraries it opened rather than objects it linked.
//
// Its own translation unit for the reason embed_run.cpp is one, inverted.
// That file is quarantined because it NAMES `bronze_main`, so anything linking
// it must define that symbol. Nothing here names a compiled symbol at all —
// the entry arrives as a pointer — which is exactly what makes these three
// usable from a host that has no linked module and may hold several loaded
// ones.

#include <atomic>
#include <cstdint>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/microtask.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
#include "runtime/sampler.h"

namespace bronze {
extern std::atomic<uint64_t> g_shapeTransitions;
}

namespace bronze::embed {

uint32_t abiFingerprint() { return BRONZE_ABI_FINGERPRINT; }

void setEnterJsHook(EnterJsHook hook) { bronze::rtSetEnterJsHook(hook); }

void setInterpretedFrameWalker(runtime::InterpretedFrameWalker walker) {
    runtime::rtSetInterpretedFrameWalker(walker);
}

namespace {
// The runtime's hook takes the op as a plain integer (the runtime does not
// see embed.h); this thread's embed-level hook is forwarded through one
// trampoline so the enum crosses exactly here.
thread_local PromiseRejectionHook t_rejectionHook = nullptr;
void rejectionTrampoline(uint32_t op, Value promise, Value reason) {
    if (t_rejectionHook != nullptr) {
        t_rejectionHook(static_cast<PromiseRejectionOperation>(op), promise, reason);
    }
}
}  // namespace

void setPromiseRejectionHook(PromiseRejectionHook hook) {
    t_rejectionHook = hook;
    runtime::rtSetRejectionHook(hook != nullptr ? &rejectionTrampoline : nullptr);
}

void runEntry(ModuleEntry entry) {
    if (entry == nullptr) return;
    // The thread that runs a module's entry is the thread that runs its
    // compiled JS from then on — the fact the sampling profiler
    // (BRONZE_SAMPLE=1) needs and the one place a loaded module's home
    // thread is knowable. A no-op unless the sampler is armed.
    runtime::samplerNoteJsThread();
    // The root frame runMain opens, for the same reason: Rooted<> handles
    // inside runtime helpers register here, and a host calling in from its own
    // frame loop has no bronze frame on the stack. Generated code links its
    // own contiguous slot frames onto its thread's ABI-block frame_top
    // separately.
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
void endModuleLoad(ModuleHandle module) { runtime::rtEndModuleEpoch(module); }
void unloadModule(ModuleHandle module) { runtime::rtDropModuleEpoch(module); }

void collectGarbage() { runtime::rtHeap().collect(); }

uint64_t relocationEpoch() { return runtime::rtHeap().relocation_epoch(); }

void bindThreadHeap() { (void)runtime::rtHeap(); }

void setProfileCalleeNamer(ProfileCalleeNamer namer) {
    runtime::profileSetCalleeNamer(namer);
}

void dumpProfileReport() {
    runtime::dumpProfileReport();
}

RuntimeTelemetry getRuntimeTelemetry() {
    RuntimeTelemetry tel;
    auto& heap = runtime::rtHeap();
    tel.heapUsedBytes = heap.used_size();
    tel.heapCommittedBytes = heap.committed_size();
    tel.heapReservedBytes = heap.reserved_size();
    tel.gcCollections = heap.collection_count();
    tel.gcPauseNs = heap.last_pause_ns();
    tel.shapeTransitions = bronze::g_shapeTransitions.load(std::memory_order_relaxed);
    return tel;
}

}  // namespace bronze::embed
