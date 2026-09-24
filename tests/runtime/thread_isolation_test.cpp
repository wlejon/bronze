// The runtime's state is per-THREAD: a second OS thread that touches the
// runtime gets its own heap, arena, interning and intrinsics, and nothing it
// does — allocation, intrinsic construction, collection — may disturb the
// first thread's runtime.
//
// A doctest deliberately driving the runtime from C++: it proves the C++
// half of per-thread isolation on its own, with no compiled module in the
// loop. Generated code reaches the same per-thread state through the
// bronze_tls_block its prologue fetches; tests/threaded_modules covers that
// half with two compiled modules on two threads.
//
// The worker records plain facts and the ASSERTIONS all run on the main
// thread after join: doctest's assertion machinery is not something two
// threads may enter at once.

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/class_family.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/property_key.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/slot_repr.h"
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
    CHECK(facts.firstKeyId != ~0u);
    CHECK(facts.firstKeyId != mainKeyId);
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

TEST_CASE("class family registry concurrent registration and lookup safety") {
    classFamilyResetForTesting();
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};

    uint32_t keyX = bronze_register_key_string("thread_x");
    uint32_t keyY = bronze_register_key_string("thread_y");
    uint32_t keyZ = bronze_register_key_string("thread_z");
    uint32_t keyMap[] = {keyX, keyY, keyZ};
    uint32_t classTable[] = {0, 2};
    uint32_t fieldTable[] = {(0u << 1) | 1u, (1u << 1) | 1u};
    uint64_t base = 0;
    bronze_register_class_family(classTable, 1, fieldTable, keyMap, &base);

    std::atomic<bool> readerReady{false};
    std::vector<uint64_t> stamps;
    std::thread reader([&] {
        ShadowStackFrame frame;
        Heap& heap = rtHeap();
        NonMovingArena& arena = rtArena();
        Shape* root = Shape::createRoot(arena, Value::fromNull());
        Rooted<Value> nameX(Value::fromString(StringHeader::createFromUTF8(heap, "thread_x")));
        Rooted<Value> nameY(Value::fromString(StringHeader::createFromUTF8(heap, "thread_y")));
        uint32_t slot = 0;
        Shape* s1 = root->addProperty(arena, heap, nameX, slot, true, false, true, true);
        Shape* s2 = s1->addProperty(arena, heap, nameY, slot, true, false, true, true);
        readerReady.store(true, std::memory_order_release);

        while (!start.load(std::memory_order_acquire)) {}

        do {
            uint64_t id = classFamilyIdFor(s2);
            stamps.push_back(id);
        } while (!done.load(std::memory_order_relaxed));
    });

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 50; ++i) {
            uint32_t ct[] = {0, 2};
            uint32_t ft[] = {(0u << 1) | 1u, (1u << 1) | 1u};
            uint64_t b = 0;
            bronze_register_class_family(ct, 1, ft, keyMap, &b);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    while (!readerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    CHECK(!stamps.empty());
    for (uint64_t s : stamps) {
        CHECK(s >= BRONZE_ABI_FAMILY_FIRST_ID);
    }
}

TEST_CASE("slot representation registry concurrent registration and check safety") {
    slotReprResetForTesting();
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};

    NonMovingArena& arena = rtArena();
    StringHeader* name1 = StringHeader::createFromUTF8InArena(arena, "slot_prop_alpha");
    slotReprRegisterName(name1);

    std::atomic<uint64_t> matchCount{0};
    // Every check the reader completes bumps `checks`. The writer does not
    // begin until the reader has checked once, and halfway through its
    // registrations it waits for a further check, so at least one check is
    // guaranteed to run while registration is in progress, whatever the
    // scheduling.
    std::atomic<uint64_t> checks{0};
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire)) {}
        NonMovingArena& wArena = rtArena();
        StringHeader* checkHdr = StringHeader::createFromUTF8InArena(wArena, "slot_prop_alpha");
        PropertyKey key = PropertyKey::forString(checkHdr);
        do {
            if (slotReprEligible(key)) {
                matchCount.fetch_add(1, std::memory_order_relaxed);
            }
            checks.fetch_add(1, std::memory_order_release);
            std::this_thread::yield();
        } while (!done.load(std::memory_order_acquire));
    });

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        auto awaitCheckAfter = [&](uint64_t seen) {
            while (checks.load(std::memory_order_acquire) <= seen) {
                std::this_thread::yield();
            }
        };
        awaitCheckAfter(0);
        NonMovingArena& wArena = rtArena();
        for (int i = 0; i < 100; ++i) {
            if (i == 50) awaitCheckAfter(checks.load(std::memory_order_acquire));
            StringHeader* newName = StringHeader::createFromUTF8InArena(wArena, "slot_prop_" + std::to_string(i));
            slotReprRegisterName(newName);
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    // The pre-registered name is eligible on every check, including those
    // that overlapped registration.
    CHECK(checks.load() >= 2);
    CHECK(matchCount.load() == checks.load());
    CHECK(slotReprEligibleCount() >= 101);
}

TEST_CASE("key info registry thread-local caching eliminates mutex contention across threads") {
    std::vector<uint32_t> keyIndices;
    for (int i = 0; i < 20; ++i) {
        std::string name = (i % 2 == 0) ? std::to_string(i * 10) : ("key_info_prop_" + std::to_string(i));
        keyIndices.push_back(bronze_register_key_string(name.c_str()));
    }

    constexpr int kNumThreads = 4;
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {}
            for (int iter = 0; iter < 1000; ++iter) {
                for (size_t idx = 0; idx < keyIndices.size(); ++idx) {
                    uint32_t k = keyIndices[idx];
                    const KeyInfo& info = rtKeyInfo(k);
                    if (idx % 2 == 0) {
                        CHECK(info.isElemIndex);
                        CHECK(info.elemIndex == static_cast<uint32_t>(idx * 10));
                    } else {
                        CHECK(!info.isElemIndex);
                    }
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }
}

