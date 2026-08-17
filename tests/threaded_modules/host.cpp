// Two compiled bronze modules on two threads, CONCURRENTLY, one process —
// the executable form of embed.h's threading contract, and the generated-code
// half of what tests/runtime/thread_isolation_test.cpp proves for the C++
// runtime alone.
//
// What each piece is proving:
//
//  * No announce step. Neither thread calls anything before its module's
//    entry; the entry's first allocation is what builds that thread's
//    runtime, lazily, which is the contract's opening promise.
//
//  * Generated code reaches PER-THREAD state. Each entry's prologue fetches
//    bronze_tls_block for its own thread, registers its module's .data spans
//    with that thread's collector, interns its keys into that thread's
//    tables. The hammer loop then collects on each thread after every call
//    while the other thread is mid-allocation — a root registered with the
//    WRONG thread's heap answers a wrong string here, not a crash somewhere.
//
//  * Isolation is total. B's thread reads the name A published and must get
//    undefined: two globalThis objects, two heaps, nothing shared. (Values
//    are thread-bound, so each thread converts its results to std::string on
//    its OWN stack; the main thread prints both after the join — also why the
//    modules never touch stdout.)
//
// Open-coded entry sequence rather than embed::runEntry, two_module's
// pattern: the ABI stamps are checked once, up front, on the main thread —
// they are link-time constants, and checking before ANY thread runs is what
// a loader would do.

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

extern "C" void bronze_thread_a();
extern "C" void bronze_thread_b();
extern "C" const uint32_t bronze_thread_a_abi_fingerprint;
extern "C" const uint32_t bronze_thread_b_abi_fingerprint;

namespace {

constexpr int kHammerIters = 64;

// Everything a worker learned, as PLAIN C++ data: the values these came from
// are thread-bound and die with the worker's last frame, so the conversion to
// std::string happens on the owning thread and only bytes cross the join.
struct Report {
    std::string summary;
    int hammerIters = 0;
    int hammerFailures = 0;
    std::string firstMismatch;
    bool isolated = false;
};

// The global object through the same two calls generated code makes — against
// THIS thread's key table and THIS thread's global, which is the point.
bronze::Value globalObject() {
    const uint32_t key = bronze_register_key_string("globalThis");
    return bronze::Value(bronze_global_get(key, nullptr));
}

void runModule(void (*entry)(), const char* summaryName, const char* hammerName,
               const char* otherSummaryName, char prefix, int itemCount, Report& out) {
    // The first bronze anything on this thread: the frame, then the entry.
    bronze::ShadowStackFrame root_frame;
    entry();
    bronze::runtime::rtDrainMicrotasks();

    bronze::embed::Persistent summary{
        bronze::embed::getProperty(globalObject(), summaryName)};
    // Everything the module built moves; the Persistent must follow it on
    // this thread's registry.
    bronze::runtime::rtHeap().collect();
    out.summary = bronze::embed::toUtf8(summary.get());

    // The OTHER module's published name: undefined here, whatever the other
    // thread has or has not done yet, because this thread's globalThis has
    // never met it.
    out.isolated =
        bronze::embed::isUndefined(bronze::embed::getProperty(globalObject(), otherSummaryName));

    bronze::embed::Persistent hammer{bronze::embed::getProperty(globalObject(), hammerName)};
    for (int i = 0; i < kHammerIters; ++i) {
        const bronze::Value arg = bronze::embed::fromDouble(i);
        bronze::embed::CallResult result =
            bronze::embed::call(hammer.get(), bronze::embed::undefined(), {&arg, 1});
        ++out.hammerIters;

        char want[64];
        std::snprintf(want, sizeof want, "%c%d#%d#%c%d:%d", prefix, i, itemCount, prefix, i,
                      itemCount - 1);
        const std::string got =
            result.thrown ? std::string("<thrown>") : bronze::embed::toUtf8(result.value);
        if (got != want) {
            if (out.hammerFailures == 0) {
                out.firstMismatch = "iter " + std::to_string(i) + " got " + got + " want " + want;
            }
            ++out.hammerFailures;
        }
        // A full collection per iteration ON THIS THREAD, while the other
        // thread allocates: the concurrency this whole test exists for.
        bronze::runtime::rtHeap().collect();
    }
}

void printReport(char prefix, const Report& r) {
    std::printf("%c summary=%s\n", prefix, r.summary.c_str());
    std::printf("%c hammer iters=%d failures=%d isolated=%d\n", prefix, r.hammerIters,
                r.hammerFailures, r.isolated ? 1 : 0);
    // Byte-compared against the pinned expectation, so a mismatch line is a
    // test failure by construction — it only exists to say WHAT diverged.
    if (r.hammerFailures > 0) {
        std::printf("%c FIRST MISMATCH %s\n", prefix, r.firstMismatch.c_str());
    }
}

}  // namespace

int main() {
    bronze::embed::setupIo();
    bronze::runtime::rtCheckObjectAbi(bronze_thread_a_abi_fingerprint);
    bronze::runtime::rtCheckObjectAbi(bronze_thread_b_abi_fingerprint);

    Report reportA;
    Report reportB;

    // B on its own thread, A on this one, at the same time.
    std::thread worker(
        [&reportB] { runModule(bronze_thread_b, "summaryB", "hammerB", "summaryA", 'B', 30, reportB); });
    runModule(bronze_thread_a, "summaryA", "hammerA", "summaryB", 'A', 40, reportA);
    worker.join();

    printReport('A', reportA);
    printReport('B', reportB);
    return 0;
}
