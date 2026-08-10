#include "runtime/fatal.h"

#include <cstdio>
#include <cstdlib>

namespace bronze {

static FatalHandler g_fatalHandler = nullptr;

void setFatalHandler(FatalHandler handler) {
    g_fatalHandler = handler;
}

[[noreturn]] void fatal(const char* msg) {
    if (g_fatalHandler) {
        g_fatalHandler(msg);
    }
    std::fprintf(stderr, "Hard runtime error: %s\n", msg);
    std::abort();
}

}  // namespace bronze
