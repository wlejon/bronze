// Running out of heap is a catchable RangeError, not a dead process.
//
// The program below keeps everything it allocates until the heap's old
// generation is full. brass (gc::Heap) refuses the allocation that would eat
// into the room a collection needs for promotion, after a full collection has
// failed to free enough; bronze turns that refusal into a RangeError thrown at
// the allocation site. The program catches it, lets go, and carries on, which
// is the whole contract: the engine is still usable afterwards.
//
// BRASS_GC_OLD_RESERVE_MB shrinks the old generation's reservation from 4 GB
// to something a test can fill in a second or two.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../test_temp_dir.h"
#include "cli/driver.h"

namespace {

std::filesystem::path oomWorkDir() {
    const std::filesystem::path dir = bronze_test::tempDir() / "bronze_out_of_memory_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string runWithSmallHeap(const std::filesystem::path& exePath) {
#ifdef _WIN32
    _putenv_s("BRASS_GC_OLD_RESERVE_MB", "256");
    FILE* pipe = _popen(exePath.string().c_str(), "r");
#else
    setenv("BRASS_GC_OLD_RESERVE_MB", "256", 1);
    FILE* pipe = popen(exePath.string().c_str(), "r");
#endif
    std::string result;
    if (pipe) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result += buffer;
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }
#ifdef _WIN32
    _putenv_s("BRASS_GC_OLD_RESERVE_MB", "");
#else
    unsetenv("BRASS_GC_OLD_RESERVE_MB");
#endif
    return result;
}

std::string buildAndRunSmallHeap(const std::string& name, const std::string& source) {
    const std::filesystem::path dir = oomWorkDir();
    const std::filesystem::path js = dir / (name + ".js");
    const std::filesystem::path exe = dir / (name + ".exe");
    {
        std::ofstream out(js, std::ios::binary);
        out << source;
    }
    std::error_code ec;
    std::filesystem::remove(exe, ec);
    std::string err;
    const int status = bronze::cli::runBuild(js.string(), exe.string(), &err);
    REQUIRE_MESSAGE(status == 0, err);
    return runWithSmallHeap(exe);
}

}  // namespace

TEST_CASE("an exhausted heap throws a catchable RangeError and the program goes on") {
    const std::string out = buildAndRunSmallHeap("oom_catch",
        "let hold = [];\n"
        "let caught = null;\n"
        "try {\n"
        "  for (let i = 0; i < 100000000; i++) hold.push({ i, pair: [i, i + 1], tag: 'x' + (i & 1023) });\n"
        "} catch (e) { caught = e; }\n"
        "console.log('range=' + (caught instanceof RangeError));\n"
        "console.log('message=' + (caught && caught.message));\n"
        "const held = hold.length;\n"
        "console.log('held=' + (held > 100000));\n"
        // What was built before the failure is intact.
        "let ok = true;\n"
        "for (let i = 0; i < held; i += 9973) if (hold[i].i !== i || hold[i].pair[1] !== i + 1) ok = false;\n"
        "console.log('intact=' + ok);\n"
        "hold = null;\n"
        // Let go, and the heap is usable again: a second fill of half the size.
        "let again = [];\n"
        "for (let i = 0; i < (held >> 1); i++) again.push({ i });\n"
        "console.log('again=' + (again.length === (held >> 1)));\n"
        // And a second failure is caught just the same.
        "let second = null;\n"
        "try {\n"
        "  for (let i = 0; i < 100000000; i++) again.push({ i, pair: [i] });\n"
        "} catch (e) { second = e; }\n"
        "console.log('second=' + (second instanceof RangeError));\n");

    CHECK(out.find("range=true\n") != std::string::npos);
    CHECK(out.find("message=out of memory\n") != std::string::npos);
    CHECK(out.find("held=true\n") != std::string::npos);
    CHECK(out.find("intact=true\n") != std::string::npos);
    CHECK(out.find("again=true\n") != std::string::npos);
    CHECK(out.find("second=true\n") != std::string::npos);
}
