// Program entry for an embedding host: what src/rt/rt.cpp's `main` does,
// callable after the host has registered its globals and functions.
//
// Its own translation unit, deliberately: `bronze_main` is defined by the
// COMPILED PROGRAM's object file, which the embed unit tests do not link. A
// static library only surfaces an unresolved external when the object that
// names it is pulled in, so keeping the one reference to `bronze_main` here —
// and out of every function the tests call — is what lets the same
// bronze_embed.lib serve both a real host and a runtime-level test binary.

#include <cstdint>

#include "embed/embed.h"
#include "runtime/abi_guard.h"
#include "runtime/gc.h"
#include "runtime/microtask.h"
#include "runtime/sampler.h"

extern "C" void bronze_main();
extern "C" const uint32_t bronze_object_abi_fingerprint;

namespace bronze::embed {

void runMain() {
    // The object's ABI stamp against this runtime's, before any compiled code
    // runs. The reference to the object's symbol lives in this TU under the
    // same quarantine as bronze_main above — a host that adopted a stale
    // object dies here with both fingerprints named, not thirty seconds into
    // a helper reading a parameter the object never passed.
    runtime::rtCheckObjectAbi(bronze_object_abi_fingerprint);
    // The statically-linked twin of runEntry's note: this thread runs the
    // program's compiled JS. A no-op unless BRONZE_SAMPLE=1 armed the sampler.
    runtime::samplerNoteJsThread();
    // Root frame for the program's top level: Rooted<> handles inside runtime
    // helpers register here, exactly as under the standalone main. Generated
    // code registers its own contiguous slot frames separately.
    bronze::ShadowStackFrame root_frame;
    bronze_main();
    // The same checkpoint src/rt/rt.cpp's `main` performs, and inside the same
    // root frame for the same reason. A host with a frame loop pumps the queue
    // again between frames (drainMicrotasks below); a host that only runs the
    // program gets the standalone behaviour without asking for it, because a
    // program whose top level queued a job has not finished running until the
    // job has.
    runtime::rtDrainMicrotasks();
}

void runProgram() {
    setupIo();
    runMain();
}

// `drainMicrotasks`, `microtasksPending` and `setupIo` are deliberately NOT
// here: this translation unit is the one that names `bronze_main`, and a caller
// of any of them would pull this object in and fail to link (the file header
// says why the reference is quarantined). The first two live in embed.cpp, and
// `setupIo` — which a multi-module host needs and `bronze_main` is exactly what
// such a host does not have — lives in embed_io.cpp.

}  // namespace bronze::embed
