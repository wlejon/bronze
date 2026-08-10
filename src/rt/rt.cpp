#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

extern "C" void bronze_main();

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    bronze_main();
    return 0;
}
