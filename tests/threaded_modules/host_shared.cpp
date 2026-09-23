// ONE compiled module image entered on two threads CONCURRENTLY — the case
// host.cpp's two-image test does not reach. An embedder that links its JS
// once (bro's built-in modules, a worker pool running one script) enters the
// same image on every thread that needs it, so everything the image holds in
// its own writable data — inline caches, the provided-global cache, template
// cells, the module environment cell, the import table — has to be per
// thread (bronze_abi.h, bronze_module_instance). When it was per image, the
// second thread's entry and hammer latched its own heap's objects into cells
// the first thread read, both collectors forwarded the same cells, and under
// BRONZE_GC_STRESS + BRONZE_GC_POISON a thread called into the other's
// poisoned from-space.
//
// Each thread runs the entry, then calls the module's hammer 64 times with a
// full collection of its own heap after every call while the other thread
// does the same. Every answer is checked against what THIS thread's calls
// alone produce — the call count lives in the module environment, the
// template strings object must be this thread's own — so a table shared
// across threads shows up as a wrong string, and one a thread's collector
// failed to forward as a crash.

#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/abi_guard.h"
#include "runtime/gc.h"
#include "runtime/microtask.h"
#include "runtime/rt_state.h"

extern "C" void bronze_shared_mod();
extern "C" const uint32_t bronze_shared_mod_abi_fingerprint;

namespace {

constexpr int kHammerIters = 64;

struct Report {
    std::string summary;
    std::string first;
    int hammerIters = 0;
    int hammerFailures = 0;
    std::string firstMismatch;
};

bronze::Value globalObject() {
    const uint32_t key = bronze_register_key_string("globalThis");
    return bronze::Value(bronze_global_get(key, nullptr));
}

// What mod_shared.js's hammer answers for call `i` on a thread whose calls so
// far were exactly its own i calls before this one.
std::string expected(const std::string& prefix, int i) {
    const int n = i + 1;
    const long long y = 30LL * i;
    const long long len2 = (435LL * 435LL + y * y) / 1000;
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%s%d n=%d acc=435,%lld len2=%lld last=%s29(29,%d) sq=15 json=[%d,%d] "
                  "arr=true hist=%d same:call|of|:%d,%s",
                  prefix.c_str(), i, n, y, len2, prefix.c_str(), i, i, n, n, n, prefix.c_str());
    return buf;
}

void runModule(const std::string& prefix, Report& out) {
    bronze::ShadowStackFrame root_frame;
    bronze_shared_mod();
    bronze::runtime::rtDrainMicrotasks();

    bronze::embed::Persistent summary{bronze::embed::getProperty(globalObject(), "summaryShared")};
    bronze::runtime::rtHeap().collect();
    out.summary = bronze::embed::toUtf8(summary.get());

    bronze::embed::Persistent hammer{bronze::embed::getProperty(globalObject(), "hammerShared")};
    bronze::embed::Persistent prefixValue{bronze::embed::fromUtf8(prefix)};
    for (int i = 0; i < kHammerIters; ++i) {
        const bronze::Value args[2] = {prefixValue.get(), bronze::embed::fromDouble(i)};
        bronze::embed::CallResult result =
            bronze::embed::call(hammer.get(), bronze::embed::undefined(), {args, 2});
        ++out.hammerIters;
        const std::string got =
            result.thrown ? std::string("<thrown>") : bronze::embed::toUtf8(result.value);
        if (i == 0) out.first = got;
        const std::string want = expected(prefix, i);
        if (got != want) {
            if (out.hammerFailures == 0) {
                out.firstMismatch = "iter " + std::to_string(i) + " got " + got + " want " + want;
            }
            ++out.hammerFailures;
        }
        bronze::runtime::rtHeap().collect();
    }
}

void printReport(const char* name, const Report& r) {
    std::printf("%s summary=%s\n", name, r.summary.c_str());
    std::printf("%s first=%s\n", name, r.first.c_str());
    std::printf("%s hammer iters=%d failures=%d\n", name, r.hammerIters, r.hammerFailures);
    if (r.hammerFailures > 0) {
        std::printf("%s FIRST MISMATCH %s\n", name, r.firstMismatch.c_str());
    }
}

}  // namespace

int main() {
    bronze::embed::setupIo();
    bronze::runtime::rtCheckObjectAbi(bronze_shared_mod_abi_fingerprint);

    Report reportA;
    Report reportB;
    std::thread worker([&reportB] { runModule("B", reportB); });
    runModule("A", reportA);
    worker.join();

    printReport("A", reportA);
    printReport("B", reportB);
    return 0;
}
