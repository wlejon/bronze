// The runtime's state is per-THREAD: a second OS thread that touches the
// runtime gets its own heap, arena, interning and intrinsics, and nothing it
// does — allocation, intrinsic construction, collection — may disturb the
// first thread's runtime.
//
// A doctest and not an oracle case, deliberately: compiled code still reaches
// its runtime through process-global ABI symbols (bronze_gc_frame_top, the
// alloc window), so GENERATED code runs on one thread until the per-thread
// ABI block lands. What is per-thread today is the C++ runtime, and only C++
// can exercise a second thread's copy of it — which is exactly what this file
// does.
//
// The worker records plain facts and the ASSERTIONS all run on the main
// thread after join: doctest's assertion machinery is not something two
// threads may enter at once.

#include <doctest/doctest.h>

#include <string>
#include <thread>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

std::string readString(Value v) {
    return rtUtf8Chars(v.asString<StringHeader>());
}

}  // namespace

TEST_CASE("a second thread gets its own runtime and its collections leave the first alone") {
    ShadowStackFrame frame;

    // Main-thread facts, established BEFORE the worker runs.
    Heap* const mainHeap = &rtHeap();
    const uint32_t mainKeyId = bronze_register_key_string("isolationKeyA");
    (void)mainKeyId;
    SymbolHeader* const mainIteratorSymbol = rtSymbolIterator();
    Rooted<Value> keepsake{rtMakeString("main-thread-keepsake")};
    const uint64_t mainCollectionsBefore = rtHeap().collection_count();

    // What the worker observed, asserted after join.
    struct WorkerFacts {
        Heap* heap = nullptr;
        uint32_t firstKeyId = ~0u;
        SymbolHeader* iteratorSymbol = nullptr;
        uint64_t collections = 0;
        std::string survivor;
        bool globalThisIsObject = false;
    } facts;

    std::thread worker([&facts] {
        // The per-thread frame, exactly as every entry opens one (gc.h): a
        // Rooted on this thread registers into this thread's chain.
        ShadowStackFrame workerFrame;

        // FIRST intern on a fresh runtime: id 0 proves this thread's intern
        // table started empty rather than continuing the main thread's.
        facts.firstKeyId = bronze_register_key_string("isolationKeyB");
        facts.heap = &rtHeap();

        // Build real intrinsics on this thread — the global object walks the
        // whole builtin ladder, so this is the per-thread lazy-init path
        // (permanent roots, root sources, arena shapes) end to end.
        facts.globalThisIsObject = rtGlobalThisObject().isObject();
        facts.iteratorSymbol = rtSymbolIterator();

        // Allocate garbage, keep one survivor rooted, collect repeatedly:
        // this thread's collector, walking this thread's roots.
        Rooted<Value> survivor{rtMakeString("worker-thread-survivor")};
        for (int i = 0; i < 64; ++i) (void)rtMakeString("worker garbage " + std::to_string(i));
        rtHeap().collect();
        rtHeap().collect();
        facts.collections = rtHeap().collection_count();
        facts.survivor = readString(survivor.get());
    });
    worker.join();

    // The worker had a runtime of its own...
    CHECK(facts.heap != nullptr);
    CHECK(facts.heap != mainHeap);
    CHECK(facts.firstKeyId == 0);
    CHECK(facts.globalThisIsObject);
    CHECK(facts.iteratorSymbol != nullptr);
    CHECK(facts.iteratorSymbol != mainIteratorSymbol);
    CHECK(facts.collections >= 2);
    CHECK(facts.survivor == "worker-thread-survivor");

    // ...and this thread's runtime never noticed. The keepsake reads back
    // through a root the worker's collections never walked, and this heap
    // counted none of the worker's collections as its own.
    CHECK(&rtHeap() == mainHeap);
    CHECK(rtHeap().collection_count() == mainCollectionsBefore);
    CHECK(readString(keepsake.get()) == "main-thread-keepsake");
}
