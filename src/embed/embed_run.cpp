// Program entry for an embedding host: what src/rt/rt.cpp's `main` does,
// callable after the host has registered its globals and functions.
//
// Its own translation unit, deliberately: `bronze_main` is defined by the
// COMPILED PROGRAM's object file, which the embed unit tests do not link. A
// static library only surfaces an unresolved external when the object that
// names it is pulled in, so keeping the one reference to `bronze_main` here —
// and out of every function the tests call — is what lets the same
// bronze_embed.lib serve both a real host and a runtime-level test binary.

#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "embed/embed.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"

extern "C" void bronze_main();

namespace bronze::embed {

void setupIo() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Before anything can fail: a hard error must reach stderr and exit,
    // never block on a modal dialog (fatal.h). Idempotent, so a host that
    // already called it — or calls runProgram after its own setup — loses
    // nothing.
    bronze::disableCrashDialogs();
}

void runMain() {
    // Root frame for the program's top level: Rooted<> handles inside runtime
    // helpers register here, exactly as under the standalone main. Generated
    // code registers its own contiguous slot frames separately.
    bronze::ShadowStackFrame root_frame;
    bronze_main();
}

void runProgram() {
    setupIo();
    runMain();
}

}  // namespace bronze::embed
