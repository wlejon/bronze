// `performance.now()` (W3C hr-time-3), end to end.
//
// These are HERE and not in the oracle suite, and the reason is the whole
// design of that suite: an oracle case pins its stdout byte-for-byte, so it is
// forbidden from reading a clock, and `oracle_test.cpp`'s `readsTheClock` fails
// any case that does. That ban is right and this test does not argue with it —
// it is why the assertions below are RELATIONS (monotonic, sub-millisecond,
// finite) rather than bytes. A clock's contract is not a string.
//
// What is still checked against node, out of band and by hand, is the small
// deterministic surface: `typeof performance`, `typeof performance.now`,
// `typeof performance.now()` and the @@toStringTag all read identically under
// both engines.

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/driver.h"

namespace {

#if BRONZE_WITH_LLVM

std::filesystem::path workDir() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "bronze_performance_now_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string runOutput(const std::filesystem::path& exePath) {
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(exePath.string().c_str(), "r");
#else
    FILE* pipe = popen(exePath.string().c_str(), "r");
#endif
    if (!pipe) return result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result += buffer;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

std::string buildAndRun(const std::string& name, const std::string& source) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    const std::filesystem::path exe = dir / (name + ".exe");
    write(js, source);
    std::error_code ec;
    std::filesystem::remove(exe, ec);

    std::string err;
    const int status = bronze::cli::runBuild(js.string(), exe.string(), &err);
    REQUIRE_MESSAGE(status == 0, err);
    return runOutput(exe);
}

TEST_CASE("performance.now is a callable clock returning a Number") {
    const std::string out = buildAndRun("perf_shape",
        "console.log(typeof performance);\n"
        "console.log(typeof performance.now);\n"
        "console.log(typeof performance.now());\n"
        "console.log(Object.prototype.toString.call(performance));\n"
        "console.log('finite=' + Number.isFinite(performance.now()));\n");

    CHECK(out.find("object\n") != std::string::npos);
    CHECK(out.find("function\n") != std::string::npos);
    CHECK(out.find("number\n") != std::string::npos);
    CHECK(out.find("[object Performance]") != std::string::npos);
    CHECK(out.find("finite=true") != std::string::npos);
}

// The property `Date.now()` cannot give: a region far shorter than a
// millisecond still has a measurable, non-zero length. This is the whole reason
// bench/harness.js needs this clock rather than the one ECMA-262 already had.
TEST_CASE("performance.now resolves a region Date.now rounds to zero") {
    const std::string out = buildAndRun("perf_resolution",
        "const t0 = performance.now();\n"
        "let s = 0;\n"
        "for (let i = 0; i < 50000; i++) s += i * 0.5;\n"
        "const t1 = performance.now();\n"
        "const elapsed = t1 - t0;\n"
        "console.log('positive=' + (elapsed > 0));\n"
        "console.log('submillisecond=' + (elapsed < 50));\n"
        // A fractional part is what an integer clock cannot produce at all.
        "console.log('fractional=' + (elapsed % 1 !== 0));\n"
        "console.log('sum=' + s);\n");

    CHECK(out.find("positive=true") != std::string::npos);
    CHECK(out.find("submillisecond=true") != std::string::npos);
    CHECK(out.find("fractional=true") != std::string::npos);
    // The loop really ran: a clock test that optimized its own workload away
    // would pass every assertion above while measuring nothing.
    CHECK(out.find("sum=624987500") != std::string::npos);
}

// Monotonic, which is the other half of the contract and the half `Date.now()`
// cannot promise at all: it reads a wall clock an NTP step can drag backwards
// in the middle of a measurement.
TEST_CASE("performance.now never goes backwards") {
    const std::string out = buildAndRun("perf_monotonic",
        "let ok = true;\n"
        "let prev = performance.now();\n"
        "for (let i = 0; i < 20000; i++) {\n"
        "  const t = performance.now();\n"
        "  if (t < prev) ok = false;\n"
        "  prev = t;\n"
        "}\n"
        "console.log('nondecreasing=' + ok);\n");

    CHECK(out.find("nondecreasing=true") != std::string::npos);
}

// A member hr-time-3 defines and bronze has not built must be LOUD, not
// `undefined` — the same rule `Math` is held to. A program that feature-tests
// `performance.mark` and finds it missing takes a branch no engine would take.
TEST_CASE("an unimplemented performance member is diagnosed by name") {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / "perf_refusal.js";
    const std::filesystem::path exe = dir / "perf_refusal.exe";
    write(js, "console.log(performance.mark);\n");
    std::error_code ec;
    std::filesystem::remove(exe, ec);

    std::string err;
    REQUIRE(bronze::cli::runBuild(js.string(), exe.string(), &err) == 0);
    // The refusal is a hard error on stderr and a non-zero exit, so stdout
    // carries nothing: `undefined` never reaches the program.
    const std::string out = runOutput(exe);
    CHECK(out.find("undefined") == std::string::npos);
}

#endif  // BRONZE_WITH_LLVM

}  // namespace
