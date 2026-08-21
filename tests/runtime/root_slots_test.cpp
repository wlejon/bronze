// The small-buffer root blocks (runtime/root_slots.h, runtime/rt_roots.h) at
// the level JS cannot see them.
//
// tests/oracle/cases/root_block_widths.js pins the ANSWERS — every width, a
// collection through the middle of each, blocks nested inside blocks. What
// that case cannot show is where the slots LIVED, and that is the whole of
// this chunk's first mechanism: a block that quietly kept mallocing would pass
// every one of its scenarios and recover nothing. So this file asks the
// storage directly.
//
// It also pins the two properties that make the storage safe:
//   - a block is sized once and never moves, because the frame holds pointers
//     INTO it for its lifetime;
//   - a frame's LIST may move freely, because what it holds is addresses of
//     Values that live elsewhere, and the collector reads it through data()
//     every time.

#include <doctest/doctest.h>

#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/root_slots.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// `rtHeap()` first, and not incidentally: runtime start-up is LAZY and it is
// what reads the BRONZE_NO_* environment, so a flag set before the first heap
// touch is put back by it. The same trap chunk 3 hit and chunk 4 re-hit.
struct InlineRootsOn {
    InlineRootsOn() {
        (void)rtHeap();
        saved = bronze_tls_block_addr()->inline_roots_enabled;
        bronze_tls_block_addr()->inline_roots_enabled = 1;
    }
    ~InlineRootsOn() { bronze_tls_block_addr()->inline_roots_enabled = saved; }

private:
    uint64_t saved = 0;
};

// An object with one own property holding a number, so a slot that survives a
// collection can be shown to still name a LIVE object and not merely a
// plausible address.
Value tagged(double n) {
    Value obj(bronze_create_object());
    Rooted<Value> root{obj};
    const uint32_t key = bronze_register_key_string("v");
    bronze_prop_set(root.get().rawBits(), key, Value::fromDouble(n).rawBits(), nullptr, false);
    return root.get();
}

// The receiver is rooted BEFORE the key is registered, and that order is the
// point: `bronze_register_key_string` can allocate, and a raw Value received
// as a parameter and held across an allocation is the exact bug these tests
// exist to catch elsewhere. Chunk 4 wrote this helper the other way round and
// BRONZE_GC_STRESS=1 found it.
double tagOf(Value v) {
    Rooted<Value> root{v};
    const uint32_t key = bronze_register_key_string("v");
    return Value(bronze_prop_get(root.get().rawBits(), key, nullptr)).asNumber();
}

// N tagged objects, every one of them rooted the instant it exists, handed
// back as raw bits with the roots already released — which is the situation a
// callee's `argv` is in, and the only situation RootedArgs is for.
std::vector<uint64_t> unrootedArgv(uint32_t argc, double base) {
    std::vector<uint64_t> argv(argc);
    std::vector<Rooted<Value>> hold;
    hold.reserve(argc);
    for (uint32_t i = 0; i < argc; ++i) hold.emplace_back(tagged(base + static_cast<double>(i)));
    for (uint32_t i = 0; i < argc; ++i) argv[i] = hold[i].get().rawBits();
    return argv;
    // `hold` unwinds here. From this point the bits in `argv` are unrooted,
    // and nothing between here and the RootedArgs that copies them allocates
    // on the bronze heap — which is the contract, stated as code.
}

}  // namespace

TEST_CASE("an argument block holds its slots inline until it cannot") {
    ShadowStackFrame frame;
    InlineRootsOn seam;

    for (uint32_t argc : {0u, 1u, 7u, 8u}) {
        std::vector<uint64_t> argv(argc, Value::fromDouble(1.0).rawBits());
        RootedArgs args(argc, argv.data());
        CAPTURE(argc);
        CHECK(args.count() == argc);
    }

    // The widths, asked of the storage rather than of the block, because the
    // block deliberately does not expose where it lives to anything but this.
    for (uint32_t n : {0u, 1u, 7u, 8u}) {
        RootValueBlock block(n);
        CAPTURE(n);
        CHECK(block.usesInlineStorage());
    }
    for (uint32_t n : {9u, 16u, 64u}) {
        RootValueBlock block(n);
        CAPTURE(n);
        CHECK_FALSE(block.usesInlineStorage());
        CHECK(block.count() == n);
    }
}

TEST_CASE("the seam puts every block back on the C heap without changing an answer") {
    ShadowStackFrame frame;
    InlineRootsOn seam;

    bronze_tls_block_addr()->inline_roots_enabled = 0;
    CHECK_FALSE(inlineRootsEnabled());

    // Not one, not eight: with the seam down the inline buffer is not used at
    // any width, which is what makes the A/B reproduce the pre-chunk shape.
    for (uint32_t n : {1u, 4u, 8u}) {
        RootValueBlock block(n);
        CAPTURE(n);
        CHECK_FALSE(block.usesInlineStorage());
    }
    // A zero-width block still needs a non-null address and still allocates
    // nothing, seam or no seam — malloc(0) is allowed to answer null.
    RootValueBlock empty(0);
    CHECK(empty.usesInlineStorage());

    // And a frame's list starts with no capacity at all, so its first push
    // allocates the way push_back did.
    {
        RootSlotList list;
        CHECK(list.capacity() == 0);
        CHECK_FALSE(list.usesInlineStorage());
    }

    // The answers are unchanged; only where the slots live changed.
    Rooted<Value> a{tagged(11.0)};
    const uint64_t argv[3] = {a.get().rawBits(), Value::fromDouble(2.0).rawBits(),
                              Value::fromUndefined().rawBits()};
    RootedArgs args(3, argv);
    rtHeap().collect();
    CHECK(tagOf(args[0]) == 11.0);
    CHECK(args[1].asNumber() == 2.0);
    CHECK(args[2].isUndefined());
    CHECK(args[9].isUndefined());
}

TEST_CASE("a frame's slot list grows past its inline buffer and keeps every root") {
    ShadowStackFrame outer;
    InlineRootsOn seam;

    {
        RootSlotList list;
        CHECK(list.usesInlineStorage());
        CHECK(list.capacity() == kRootSlotsInline);

        std::vector<Value> slots(kRootSlotsInline + 25, Value::fromDouble(0.0));
        for (uint32_t i = 0; i < slots.size(); ++i) {
            slots[i] = Value::fromDouble(static_cast<double>(i));
            list.push(&slots[i]);
        }
        CHECK_FALSE(list.usesInlineStorage());
        CHECK(list.size() == slots.size());

        // The collector's view: every pointer still names the slot it was
        // pushed for, in order, after the list itself has moved twice.
        for (uint32_t i = 0; i < slots.size(); ++i) {
            CHECK(list.data()[i] == &slots[i]);
            CHECK(list.data()[i]->asNumber() == static_cast<double>(i));
        }

        // Popping from the top is the whole of the common case; popping out of
        // order still finds its slot, which is what Rooted<>'s move needs.
        list.pop(&slots[3]);
        CHECK(list.size() == slots.size() - 1);
        CHECK(list.data()[3] == &slots[4]);
        list.pop(&slots[slots.size() - 1]);
        CHECK(list.size() == slots.size() - 2);
        // A slot that was never pushed is a no-op, not a corruption.
        Value stray = Value::fromDouble(-1.0);
        list.pop(&stray);
        CHECK(list.size() == slots.size() - 2);
    }
}

TEST_CASE("an argument block is a real root across a collection at every width") {
    ShadowStackFrame frame;
    InlineRootsOn seam;

    for (uint32_t argc : {1u, 8u, 9u, 64u}) {
        CAPTURE(argc);
        // Built into plain stack memory and then dropped: after the block
        // copies them, the block is the ONLY thing naming these objects.
        const std::vector<uint64_t> argv = unrootedArgv(argc, 0.5);
        RootedArgs args(argc, argv.data());
        rtHeap().collect();
        rtHeap().collect();

        for (uint32_t i = 0; i < argc; ++i) {
            CHECK(tagOf(args[i]) == static_cast<double>(i) + 0.5);
        }
        // The span the embed layer reads through is the SAME storage, so it is
        // current after the flips rather than a copy taken before them.
        for (uint32_t i = 0; i < argc; ++i) {
            CHECK(args.data()[i].rawBits() == args[i].rawBits());
        }
    }
}

TEST_CASE("blocks nest, and an outer one survives everything the inner ones do") {
    ShadowStackFrame frame;
    InlineRootsOn seam;

    // The recursion a builtin re-entering JS re-entering the builtin makes:
    // an outer block live while inner blocks are built, filled, collected
    // across and destroyed. Widths chosen to straddle the inline buffer in
    // both directions.
    const std::vector<uint64_t> outerArgv = unrootedArgv(3, 100.0);
    RootedArgs outerArgs(3, outerArgv.data());

    for (int depth = 0; depth < 6; ++depth) {
        const uint32_t argc = depth % 2 == 0 ? 4u : 20u;
        const std::vector<uint64_t> inner = unrootedArgv(argc, depth * 1000.0);
        RootedArgs innerArgs(argc, inner.data());
        rtHeap().collect();
        for (uint32_t i = 0; i < argc; ++i) {
            CHECK(tagOf(innerArgs[i]) == depth * 1000.0 + i);
        }
        // The outer block is still answering, from underneath.
        CHECK(tagOf(outerArgs[0]) == 100.0);
        CHECK(tagOf(outerArgs[2]) == 102.0);
    }

    CHECK(tagOf(outerArgs[0]) == 100.0);
    CHECK(tagOf(outerArgs[1]) == 101.0);
    CHECK(tagOf(outerArgs[2]) == 102.0);
}

TEST_CASE("a runtime-built block starts undefined and stays rooted at every width") {
    ShadowStackFrame frame;
    InlineRootsOn seam;

    for (uint32_t n : {0u, 1u, 8u, 9u, 33u}) {
        CAPTURE(n);
        RootedBlock block(n);
        CHECK(block.count() == n);
        // Undefined at BOTH widths: the inline half default-constructs to it,
        // the heap half is raw bytes until RootedBlock writes it.
        const auto* bits = block.data();
        for (uint32_t i = 0; i < n; ++i) {
            CHECK(Value(bits[i]).isUndefined());
        }

        for (uint32_t i = 0; i < n; ++i) block.set(i, tagged(static_cast<double>(i) * 7.0));
        rtHeap().collect();
        for (uint32_t i = 0; i < n; ++i) {
            CHECK(tagOf(Value(block.data()[i])) == static_cast<double>(i) * 7.0);
        }
    }
}

TEST_CASE("a frame that outgrew its buffer is still the frame the collector walks") {
    // The end-to-end shape: many roots open at once, forcing the frame's list
    // onto the heap, and a collection that has to find every one of them.
    ShadowStackFrame frame;
    InlineRootsOn seam;

    std::vector<Rooted<Value>> roots;
    roots.reserve(kRootSlotsInline * 4);
    for (uint32_t i = 0; i < kRootSlotsInline * 4; ++i) {
        roots.emplace_back(tagged(static_cast<double>(i) + 1000.0));
    }
    CHECK(frame.count() >= kRootSlotsInline * 4);

    rtHeap().collect();
    rtHeap().collect();

    for (uint32_t i = 0; i < roots.size(); ++i) {
        CHECK(tagOf(roots[i].get()) == static_cast<double>(i) + 1000.0);
    }
}
