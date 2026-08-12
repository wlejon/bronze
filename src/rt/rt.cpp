#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "runtime/fatal.h"
#include "runtime/gc.h"

extern "C" void bronze_main();

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Before anything can fail: a hard error must reach stderr and exit,
    // never block on a modal dialog (see fatal.h).
    bronze::disableCrashDialogs();
    // Root frame for the whole program: Rooted<> handles inside runtime helpers
    // register here. Generated code registers its own contiguous slot frames
    // separately.
    bronze::ShadowStackFrame root_frame;
    bronze_main();
    return 0;
}
