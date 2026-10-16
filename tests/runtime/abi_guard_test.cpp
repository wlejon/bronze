// The object-vs-runtime ABI guard (runtime/abi_guard.h): a matching stamp is
// silence, a mismatched one is a fatal naming both fingerprints. The real
// stamp lives in generated objects; these tests hand the check its value
// directly, which is exactly what the two program entries do.

#include <doctest/doctest.h>

#include <chrono>
#include <stdexcept>
#include <string>

#include "runtime/abi_guard.h"
#include "runtime/fatal.h"
#include "runtime/profiler.h"

using namespace bronze;
using namespace bronze::runtime;

TEST_CASE("abi guard: a matching object fingerprint passes silently") {
    rtCheckObjectAbi(BRONZE_ABI_FINGERPRINT);
}

TEST_CASE("abi guard: a mismatched object fingerprint is a named fatal") {
    setFatalHandler([](const char* msg) { throw std::runtime_error(msg); });
    CHECK_THROWS_WITH_AS(rtCheckObjectAbi(BRONZE_ABI_FINGERPRINT ^ 1u),
                         doctest::Contains("bronze ABI mismatch"), std::runtime_error);
    setFatalHandler(nullptr);
}

#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) || \
    (defined(__APPLE__) && defined(__aarch64__)) ||                  \
    (defined(__linux__) && defined(__x86_64__))

namespace {
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
uint64_t profilerBusySpinStep(uint64_t x) {
    for (int i = 0; i < 4096; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    return x;
}
}  // namespace

TEST_CASE("sampling profiler: captures samples and resolves symbols on busy loop") {
    registerProfilerThread(kProfilerThreadMain, "main");
    ProfilerOptions startOpts;
    startOpts.hz = 1000;
    startOpts.threads = kProfilerThreadMain;

    std::string err;
    REQUIRE_MESSAGE(profilerStart(startOpts, &err), err);
    CHECK(profilerRunning());

    volatile uint64_t sink = 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
    while (std::chrono::steady_clock::now() < deadline) {
        sink = profilerBusySpinStep(sink);
    }
    (void)sink;

    ProfilerStopOptions stopOpts;
    stopOpts.callers = true;
    stopOpts.text = true;
    ProfilerResult result;
    REQUIRE_MESSAGE(profilerStop(stopOpts, result, &err), err);
    CHECK_FALSE(profilerRunning());
    CHECK(result.samples > 0);
    CHECK_FALSE(result.functions.empty());
    CHECK_FALSE(result.threads.empty());
    CHECK_FALSE(result.text.empty());

    bool foundNamedSymbol = false;
    for (const ProfilerFunction& fn : result.functions) {
        if (!fn.name.empty() && fn.name != "(unresolved)") {
            foundNamedSymbol = true;
            break;
        }
    }
    CHECK(foundNamedSymbol);
    unregisterProfilerThread();
}

#endif
