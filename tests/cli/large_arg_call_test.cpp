#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/driver.h"

namespace {

void writeTestFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void removeProgram(const std::filesystem::path& exe, std::error_code& ec) {
    std::filesystem::remove(exe, ec);
    std::filesystem::path module = exe;
#if defined(_WIN32)
    module.replace_extension(".dll");
#elif defined(__APPLE__)
    module.replace_extension(".dylib");
#else
    module.replace_extension(".so");
#endif
    std::filesystem::remove(module, ec);
}

std::string runAndCaptureOutput(const std::filesystem::path& exePath) {
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(exePath.string().c_str(), "r");
#else
    FILE* pipe = popen(exePath.string().c_str(), "r");
#endif
    if (!pipe) return result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

} // namespace

TEST_CASE("CLI large argument dynamic calls, construct, and super calls (> 16 args)") {
    std::filesystem::path jsPath = std::filesystem::temp_directory_path() / "test_large_args.js";
    std::filesystem::path exePath = std::filesystem::temp_directory_path() / "test_large_args.exe";

    std::error_code ec;
    removeProgram(exePath, ec);

    // 18 arguments to dynamic call, construct, and super
    std::string source = R"(
function add18(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18) {
    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18;
}
console.log(add18(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18));

class Ctor18 {
    constructor(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18) {
        this.sum = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18;
    }
}
const c = new Ctor18(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18);
console.log(c.sum);

class Base18 {
    constructor(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18) {
        this.total = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18;
    }
}
class Sub18 extends Base18 {
    constructor() {
        super(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18);
    }
}
const s = new Sub18();
console.log(s.total);
)";

    writeTestFile(jsPath, source);

    std::string err;
    int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err);
    REQUIRE(status == 0);
    REQUIRE(std::filesystem::exists(exePath));

    std::string output = runAndCaptureOutput(exePath);
    CHECK(output == "171\n171\n171\n");

    removeProgram(exePath, ec);
    std::filesystem::remove(jsPath, ec);
}
