#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "runtime/gc.h"

extern "C" void bronze_main();

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Root frame for the whole program: Rooted<> handles inside runtime
    // helpers register here. (Generated code's own SSA values are not yet
    // rooted — collection is only safe inside helpers; see docs/0004.)
    bronze::ShadowStackFrame root_frame;
    bronze_main();
    return 0;
}
