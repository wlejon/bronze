// HostClass handles: their brands (unwrap / isInstance across inherit()
// lineages and collections) and their destructors (one call per handle made,
// run by the collection that finds the handle dead). The last case is a
// microbenchmark, skipped unless asked for:
//
//   bronze_embed_tests -tc="*handle microbench*" --no-skip
//
// It prints the cost of making a handle, of unwrapping one, and of finalizing
// one (the collector's finalizer bookkeeping, the brand release and the
// destructor), per handle.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "embed/embed.h"
#include "embed/embed_host_class.h"
#include "runtime/heap.h"
#include "runtime/rt_state.h"

using namespace bronze;

namespace {

struct Counted {
    static inline int live = 0;
    int value;
    explicit Counted(int v) : value(v) { ++live; }
    ~Counted() { --live; }
};

embed::HostClass& baseClass() {
    static embed::HostClass c;
    return c;
}
embed::HostClass& midClass() {
    static embed::HostClass c;
    return c;
}
embed::HostClass& leafClass() {
    static embed::HostClass c;
    return c;
}
embed::HostClass& otherClass() {
    static embed::HostClass c;
    return c;
}

void installClasses() {
    baseClass().install("HcBase", 0, nullptr);
    midClass().install("HcMid", 0, nullptr);
    leafClass().install("HcLeaf", 0, nullptr);
    otherClass().install("HcOther", 0, nullptr);
    midClass().inherit(baseClass());
    leafClass().inherit(midClass());
}

}  // namespace

TEST_CASE("HostClass brands survive collections and follow a three-level lineage") {
    installClasses();
    const int before = Counted::live;
    {
        embed::Persistent leaf(leafClass().createInstance(std::make_unique<Counted>(7)));
        embed::Persistent other(otherClass().createInstance(std::make_unique<Counted>(8)));
        // Young, then promoted, then old across a full collection: the brand
        // is checked at each step.
        for (int round = 0; round < 3; ++round) {
            CHECK(leafClass().isInstance(leaf.get()));
            CHECK(midClass().isInstance(leaf.get()));
            CHECK(baseClass().isInstance(leaf.get()));
            CHECK(!otherClass().isInstance(leaf.get()));
            CHECK(!leafClass().isInstance(other.get()));
            CHECK(!baseClass().isInstance(other.get()));
            REQUIRE(baseClass().unwrap(leaf.get()) != nullptr);
            CHECK(static_cast<Counted*>(baseClass().unwrap(leaf.get()))->value == 7);
            CHECK(static_cast<Counted*>(otherClass().unwrap(other.get()))->value == 8);
            if (round == 2) runtime::rtHeap().collect();
            else runtime::rtHeap().collect_minor();
        }
        // Not handles of any class: an array, a bare handle, a number.
        CHECK(baseClass().unwrap(embed::makeArray(0)) == nullptr);
        static int bare = 0;
        embed::Persistent bareHandle(embed::makeHandle(&bare, [](void*) {}));
        CHECK(baseClass().unwrap(bareHandle.get()) == nullptr);
        CHECK(!leafClass().isInstance(embed::fromDouble(1.0)));
        CHECK(Counted::live == before + 2);
    }
    runtime::rtHeap().collect();
    CHECK(Counted::live == before);
}

TEST_CASE("each HostClass handle's destructor runs once, when that handle dies") {
    installClasses();
    // A refcounted payload several handles share (bro's node wrappers): each
    // make() is paired with exactly one destructor call, so the count returns
    // to zero once every handle has died, whichever dies first.
    struct Shared {
        int refs = 0;
    };
    static Shared shared;
    static int destroyed = 0;
    shared.refs = 0;
    destroyed = 0;
    auto release = [](void* p) {
        --static_cast<Shared*>(p)->refs;
        ++destroyed;
    };
    {
        ++shared.refs;
        embed::Persistent a(leafClass().make(&shared, release));
        {
            ++shared.refs;
            embed::Persistent b(midClass().make(&shared, release));
            CHECK(midClass().unwrap(b.get()) == &shared);
            CHECK(leafClass().unwrap(b.get()) == nullptr);  // b is a mid, not a leaf
        }
        runtime::rtHeap().collect();
        CHECK(destroyed == 1);
        CHECK(shared.refs == 1);
        // The handle still alive is still its class's.
        CHECK(leafClass().unwrap(a.get()) == &shared);
        CHECK(baseClass().unwrap(a.get()) == &shared);
    }
    runtime::rtHeap().collect();
    CHECK(destroyed == 2);
    CHECK(shared.refs == 0);
}

TEST_CASE("many young HostClass handles: every destructor runs, survivors keep their brands") {
    installClasses();
    const int before = Counted::live;
    constexpr int kCount = 20000;
    embed::Persistent keep(embed::makeArray(0));
    for (int i = 0; i < kCount; ++i) {
        embed::Persistent h(leafClass().createInstance(std::make_unique<Counted>(i)));
        if (i % 97 == 0) keep.set(embed::setElement(keep.get(), static_cast<uint32_t>(i / 97), h.get()));
    }
    runtime::rtHeap().collect_minor();
    runtime::rtHeap().collect();
    const int kept = (kCount + 96) / 97;
    CHECK(Counted::live == before + kept);
    for (int k = 0; k < kept; ++k) {
        embed::Value v = embed::getElement(keep.get(), static_cast<uint32_t>(k));
        auto* c = static_cast<Counted*>(baseClass().unwrap(v));
        REQUIRE(c != nullptr);
        CHECK(c->value == k * 97);
    }
    keep.set(embed::undefined());
    runtime::rtHeap().collect();
    CHECK(Counted::live == before);
}

TEST_CASE("HostClass handle microbench" * doctest::skip()) {
    installClasses();
    // Batches small enough that eden never fills inside one, so each batch's
    // handles die in the collection the bench runs, and its finalizer stats
    // (last_finalizers / last_finalizer_ns) are that batch's.
    constexpr int kBatch = 20000;
    constexpr int kBatches = 20;
    using Clock = std::chrono::steady_clock;
    const auto ns = [](Clock::duration d) {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    };
    struct Sum {
        double makeNs = 0, pauseNs = 0, finNs = 0, fins = 0, handles = 0;
    };
    // young: the handles die young (a minor collection finds them dead).
    // old: they are promoted first (two minor collections) and die old (a
    // full collection finds them dead).
    for (const bool old : {false, true}) {
        for (int rep = 0; rep < 3; ++rep) {
            Sum sum;
            double unwrapNs = 0;
            uint64_t sink = 0;
            runtime::rtHeap().collect();
            const int before = Counted::live;
            for (int b = 0; b < kBatches; ++b) {
                runtime::rtHeap().collect_minor();  // an empty eden for the batch
                embed::Persistent keep(embed::makeArray(kBatch));
                const auto t0 = Clock::now();
                for (int i = 0; i < kBatch; ++i) {
                    embed::Value h = leafClass().createInstance(std::make_unique<Counted>(i));
                    if (old) keep.set(embed::setElement(keep.get(), static_cast<uint32_t>(i), h));
                }
                sum.makeNs += ns(Clock::now() - t0);
                if (old) {
                    runtime::rtHeap().collect_minor();
                    runtime::rtHeap().collect_minor();
                    if (b == 0) {
                        // A three-level lineage walk (leaf -> mid -> base)
                        // per unwrap; the handles read out first so only
                        // the unwrap is timed.
                        std::vector<embed::Value> hs(kBatch);
                        for (int i = 0; i < kBatch; ++i) hs[i] = embed::getElement(keep.get(), static_cast<uint32_t>(i));
                        const auto u0 = Clock::now();
                        for (int r = 0; r < 20; ++r) {
                            for (int i = 0; i < kBatch; ++i) sink += reinterpret_cast<uintptr_t>(baseClass().unwrap(hs[i]));
                        }
                        unwrapNs = ns(Clock::now() - u0) / (20.0 * kBatch);
                    }
                    keep.set(embed::undefined());
                }
                if (old) runtime::rtHeap().collect();
                else runtime::rtHeap().collect_minor();
                const auto& st = runtime::rtHeap().gc().stats();
                sum.pauseNs += static_cast<double>(st.last_pause_ns);
                sum.finNs += static_cast<double>(st.last_finalizer_ns);
                sum.fins += static_cast<double>(st.last_finalizers);
                sum.handles += kBatch;
            }
            std::printf("handle microbench %s rep %d: make %.0f ns/handle, %s collection pause %.3f ms/batch, "
                        "%.0f of %.0f finalized in their collection, %.1f ns each (finalizer run)%s",
                        old ? "old  " : "young", rep, sum.makeNs / sum.handles, old ? "full " : "minor",
                        sum.pauseNs / kBatches / 1e6, sum.fins, sum.handles,
                        sum.fins > 0 ? sum.finNs / sum.fins : 0.0, old ? "" : "\n");
            if (old) std::printf(", unwrap %.1f ns (sink %llu)\n", unwrapNs,
                                 static_cast<unsigned long long>(sink & 1));
            CHECK(Counted::live == before);
        }
    }
}
