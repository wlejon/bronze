#include <charconv>
#include <cstdio>
#include <system_error>

extern "C" double bronze_main();

int main() {
    double result = bronze_main();
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), result);
    if (ec == std::errc{}) {
        *ptr++ = '\n';
        std::fwrite(buf, 1, static_cast<size_t>(ptr - buf), stdout);
    }
    return 0;
}
