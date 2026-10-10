// A host that links a compiled module (`--emit-obj`) against the SHARED
// runtime while also linking brass statically — the shape of a host that
// carries a compiler in-process (bro links bronze's compiler for its apps).
//
// The process then holds two copies of brass's coroutine runtime: the
// runtime image's, and the host's own. A generator or async function's stub
// calls brass_coro_create to allocate its frame, and the runtime image starts,
// suspends and resumes it. Were the name defined by the host's static brass,
// the linker would bind the module's call to that copy (a static archive is
// searched before the runtime's import library), which allocates outside the
// runtime's heap and registers the frame where the runtime never looks: the
// first `async function` call died with "brass_coro_unroot: ... names no
// frame a live heap holds (a stale handle ...)". brass therefore defines it
// only as brass_default_coro_create, and the runtime alone exports the
// canonical name (bronze_abi_functions.h, BRONZE_ABI_BRASS_SYMBOLS).
//
// The host takes the address of brass_default_coro_create, as an in-process
// JIT's symbol table does, so the object holding brass's coroutine stub is in
// this image whatever else the link pulls in.

#include <cstdint>
#include <cstdio>

#include <brass/runtime/coroutine.hpp>

#include "embed/embed.h"

extern "C" void bronze_static_brass_mod();
extern "C" const uint32_t bronze_static_brass_mod_abi_fingerprint;

namespace embed = bronze::embed;

int main() {
    embed::setupIo();
    if (bronze_static_brass_mod_abi_fingerprint != embed::abiFingerprint()) {
        std::fprintf(stderr, "static-brass host: the module's ABI stamp is not the runtime's\n");
        return 1;
    }
    // The compiler in-process: brass's own coroutine entry, linked here.
    volatile auto inProcessCompiler = &brass_default_coro_create;
    (void)inProcessCompiler;

    embed::runEntry(bronze_static_brass_mod);
    embed::collectGarbage();

    embed::GlobalValue fromHost = embed::globalValue("fromHost");
    if (!fromHost.found) {
        std::fprintf(stderr, "static-brass host: the module defined no fromHost\n");
        return 1;
    }
    embed::CallResult r = embed::call(fromHost.value, embed::undefined(), {});
    if (r.thrown) {
        std::printf("host fromHost THREW\n");
        return 1;
    }
    embed::drainMicrotasks();
    embed::collectGarbage();
    std::printf("host done\n");
    return 0;
}
