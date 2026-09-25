#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../test_temp_dir.h"
#include "cli/driver.h"
// A bundled library is one large module: closures nested a dozen levels and
// more, and a top level of thousands of statements that becomes a single
// function of tens of thousands of blocks. Both used to cost far more than
// their size — inference re-walked every nested function inside every walk
// of its parent (exponential in nesting depth), and the backend held
// per-block bit sets over every register, kept every folded constant live
// from the entry block, and rescanned every block per trace or per edge;
// range analysis recursed down a dominator tree as deep as the top level is
// long — so a 41k-line esbuild bundle never finished compiling. This builds a
// generated module of that shape, in-process on a default-sized stack, and
// bounds the time it takes.

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

// Arrow callbacks nested `depth` deep, each writing its parent's variable
// (a captured cell) — the shape of a parser-combinator library.
std::string nestedClosures(int depth) {
    std::string s = "function nest(x) {\n  let a0 = x;\n";
    for (int d = 1; d <= depth; ++d) {
        const std::string cur = std::to_string(d), prev = std::to_string(d - 1);
        s += "  const f" + cur + " = (y) => {\n    let a" + cur + " = a" + prev + " + y;\n    a" +
             prev + " = a" + cur + " - y;\n";
    }
    s += "    return a" + std::to_string(depth) + ";\n";
    for (int d = depth; d >= 1; --d) {
        s += "  };\n  ";
        s += d > 1 ? "return f" + std::to_string(d) + "(1) + a" + std::to_string(d - 1) + ";\n"
                   : "const r = f1(1);\n";
    }
    s += "  return r;\n}\nconsole.log(nest(0));\n";
    return s;
}

// `statements` branches on the top level, each with its own constant.
std::string longTopLevel(int statements) {
    std::string s = "const o = { v: 0 };\nlet t = 0;\n";
    for (int i = 0; i < statements; ++i) {
        s += "if (o.v === " + std::to_string(i % 7) + ") t += " + std::to_string(i) +
             "; else o.v = " + std::to_string(i % 5) + ";\n";
    }
    s += "console.log(t, o.v);\n";
    return s;
}

} // namespace

TEST_CASE("CLI a bundle-sized module compiles in bounded time") {
#ifdef NDEBUG
    const int statements = 3000;
#else
    const int statements = 300;  // an unoptimized build is not timed
#endif
    const int depth = 20;

    std::filesystem::path jsPath = bronze_test::tempDir() / "test_large_module.js";
    std::filesystem::path exePath = bronze_test::tempDir() / "test_large_module.exe";
    std::error_code ec;
    removeProgram(exePath, ec);
    writeTestFile(jsPath, nestedClosures(depth) + longTopLevel(statements));

    const auto start = std::chrono::steady_clock::now();
    std::string err;
    const int status = bronze::cli::runBuild(jsPath.string(), exePath.string(), &err);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO("build took " << seconds << " s: " << err);
    REQUIRE(status == 0);
#ifdef NDEBUG
    // About ten seconds in a Release build; minutes, or never, before.
    CHECK(seconds < 90.0);
#else
    (void)seconds;
#endif

    // What the module prints, computed the way it computes it.
    long long t = 0;
    int v = 0;
    for (int i = 0; i < statements; ++i) {
        if (v == i % 7) t += i;
        else v = i % 5;
    }
    const std::string expected = std::to_string(depth * (depth + 1) / 2) + "\n" +
                                 std::to_string(t) + " " + std::to_string(v) + "\n";
    CHECK(runAndCaptureOutput(exePath) == expected);

    removeProgram(exePath, ec);
    std::filesystem::remove(jsPath, ec);
}
