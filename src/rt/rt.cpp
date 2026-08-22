#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "runtime/abi_guard.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/microtask.h"
#include "runtime/sampler.h"

extern "C" void bronze_main();
extern "C" const uint32_t bronze_object_abi_fingerprint;

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Before anything can fail: a hard error must reach stderr and exit,
    // never block on a modal dialog (see fatal.h).
    bronze::disableCrashDialogs();
    // The object's ABI stamp against this runtime's, before anything from the
    // object runs (abi_guard.h says why; the symbol reference lives in this TU
    // for the same reason bronze_main's does).
    bronze::runtime::rtCheckObjectAbi(bronze_object_abi_fingerprint);
    // Root frame for the whole program: Rooted<> handles inside runtime helpers
    // register here. Generated code registers its own contiguous slot frames
    // separately.
    bronze::ShadowStackFrame root_frame;
    // This thread runs the program's compiled JS — the sampling profiler's
    // target (BRONZE_SAMPLE=1; a no-op otherwise).
    bronze::runtime::samplerNoteJsThread();
    bronze_main();
    // The synchronous half of the program is over; the promise jobs it queued
    // are the rest of it. bronze has no event loop — no timers, no IO — so ONE
    // drain to quiescence here is the whole of HTML's "perform a microtask
    // checkpoint", and running it inside the root frame is what keeps a
    // suspended async frame rooted while its resumption allocates.
    //
    // The drain reports every rejection nothing ever handled, on STDERR, and
    // the process still exits 0: a program that printed its output and dropped
    // a promise exits the way it observably behaved (microtask.cpp says why at
    // length, beside the report).
    bronze::runtime::rtDrainMicrotasks();
    return 0;
}
