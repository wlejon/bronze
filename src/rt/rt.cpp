#include <charconv>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

extern "C" double bronze_main();

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    double result = bronze_main();
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), result);
    if (ec == std::errc{}) {
        *ptr++ = '\n';
        std::fwrite(buf, 1, static_cast<size_t>(ptr - buf), stdout);
    }
    return 0;
}
