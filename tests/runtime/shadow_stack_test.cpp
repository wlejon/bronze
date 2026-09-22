// Tests for shadow stack allocation, segmented overflow handling, independent
// overflow frame storage, root preservation across nested/recursive calls, and
// GC collection root traversal and forwarding.

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

struct ShadowStackTestGuard {
    ShadowStackFrame root_frame;
    ShadowStackTestGuard() {
        resetShadowStackCapacityForTesting();
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
    }
    ~ShadowStackTestGuard() {
        resetShadowStackCapacityForTesting();
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
    }
};

Value makeTaggedObject(double n) {
    Value obj(bronze_create_object());
    Rooted<Value> root{obj};
    const uint32_t key = bronze_register_key_string("test_val");
    bronze_prop_set(root.get().rawBits(), key, Value::fromDouble(n).rawBits(), nullptr, false);
    return root.get();
}

double getTaggedValue(Value v) {
    Rooted<Value> root{v};
    const uint32_t key = bronze_register_key_string("test_val");
    return Value(bronze_prop_get(root.get().rawBits(), key, nullptr)).asNumber();
}

}  // namespace

TEST_CASE("shadow stack basic frame push and pop within capacity") {
    ShadowStackTestGuard guard;

    CHECK(bronze_tls_block_addr()->frame_top == nullptr);

    bronze_gc_frame* frame1 = bronze_gc_frame_push(3);
    REQUIRE(frame1 != nullptr);
    CHECK(isShadowStackFrame(frame1));
    CHECK(frame1->prev == nullptr);
    CHECK(frame1->count == 3);
    CHECK(bronze_tls_block_addr()->frame_top == frame1);

    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(frame1->slots[i] == BRONZE_ABI_UNDEFINED_BITS);
        frame1->slots[i] = 100 + i;
    }

    bronze_gc_frame* frame2 = bronze_gc_frame_push(2);
    REQUIRE(frame2 != nullptr);
    CHECK(isShadowStackFrame(frame2));
    CHECK(frame2->prev == frame1);
    CHECK(frame2->count == 2);
    CHECK(bronze_tls_block_addr()->frame_top == frame2);

    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(frame2->slots[i] == BRONZE_ABI_UNDEFINED_BITS);
        frame2->slots[i] = 200 + i;
    }

    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == frame1);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(frame1->slots[i] == 100 + i);
    }

    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == nullptr);
}

TEST_CASE("shadow stack overflow allocates independent frames and preserves slots across nested calls") {
    ShadowStackTestGuard guard;

    // Set capacity to 4 words (Frame 1 capacity) so frames 2, 3, and 4 will overflow to independent storage.
    setShadowStackCapacityForTesting(4);

    // Frame 1: 2 slots + 2 header words = 4 words <= 4 words capacity -> fits on shadow stack and fills it.
    bronze_gc_frame* f1 = bronze_gc_frame_push(2);
    REQUIRE(f1 != nullptr);
    CHECK(isShadowStackFrame(f1));
    CHECK(f1->prev == nullptr);
    f1->slots[0] = 0x1111;
    f1->slots[1] = 0x1112;

    // Frame 2: 4 slots + 2 header words = 6 words. 4 + 6 = 10 > 8 -> overflow.
    bronze_gc_frame* f2 = bronze_gc_frame_push(4);
    REQUIRE(f2 != nullptr);
    CHECK_FALSE(isShadowStackFrame(f2));
    CHECK(f2->prev == f1);
    CHECK(bronze_tls_block_addr()->frame_top == f2);
    f2->slots[0] = 0x2221;
    f2->slots[1] = 0x2222;
    f2->slots[2] = 0x2223;
    f2->slots[3] = 0x2224;

    // Frame 3: 2 slots + 2 header words = 4 words. Stack is full -> overflow.
    bronze_gc_frame* f3 = bronze_gc_frame_push(2);
    REQUIRE(f3 != nullptr);
    CHECK_FALSE(isShadowStackFrame(f3));
    CHECK(f3->prev == f2);
    CHECK(bronze_tls_block_addr()->frame_top == f3);
    f3->slots[0] = 0x3331;
    f3->slots[1] = 0x3332;

    // Frame 4: 1 slot + 2 header words = 3 words -> overflow.
    bronze_gc_frame* f4 = bronze_gc_frame_push(1);
    REQUIRE(f4 != nullptr);
    CHECK_FALSE(isShadowStackFrame(f4));
    CHECK(f4->prev == f3);
    CHECK(bronze_tls_block_addr()->frame_top == f4);
    f4->slots[0] = 0x4441;

    // Verify critical fix: each overflow frame must have its own independent storage!
    CHECK(f2 != f3);
    CHECK(f3 != f4);
    CHECK(f2 != f4);

    // Verify all slots across all frames remain untouched and uncorrupted.
    CHECK(f1->slots[0] == 0x1111);
    CHECK(f1->slots[1] == 0x1112);

    CHECK(f2->slots[0] == 0x2221);
    CHECK(f2->slots[1] == 0x2222);
    CHECK(f2->slots[2] == 0x2223);
    CHECK(f2->slots[3] == 0x2224);

    CHECK(f3->slots[0] == 0x3331);
    CHECK(f3->slots[1] == 0x3332);

    CHECK(f4->slots[0] == 0x4441);

    // Pop f4: should restore f3 as frame_top and recycle f4 cleanly.
    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == f3);
    CHECK(f3->slots[0] == 0x3331);
    CHECK(f3->slots[1] == 0x3332);
    CHECK(f2->slots[0] == 0x2221);
    CHECK(f1->slots[0] == 0x1111);

    // Pop f3: restores f2 as frame_top.
    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == f2);
    CHECK(f2->slots[0] == 0x2221);
    CHECK(f2->slots[1] == 0x2222);
    CHECK(f2->slots[2] == 0x2223);
    CHECK(f2->slots[3] == 0x2224);
    CHECK(f1->slots[0] == 0x1111);

    // Pop f2: restores f1 as frame_top.
    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == f1);
    CHECK(f1->slots[0] == 0x1111);
    CHECK(f1->slots[1] == 0x1112);

    // Pop f1: restores frame_top to nullptr.
    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == nullptr);
}

TEST_CASE("shadow stack overflow deep recursion preserves root slots") {
    ShadowStackTestGuard guard;

    // Small capacity so that recursion enters overflow immediately.
    setShadowStackCapacityForTesting(6);

    constexpr int kMaxDepth = 30;
    std::function<void(int)> recurse = [&](int depth) {
        if (depth == 0) return;

        bronze_gc_frame* frame = bronze_gc_frame_push(3);
        REQUIRE(frame != nullptr);
        frame->slots[0] = 0xAA000000ULL + depth;
        frame->slots[1] = 0xBB000000ULL + depth;
        frame->slots[2] = 0xCC000000ULL + depth;

        recurse(depth - 1);

        // Verify that returning from inner recursion left our slots completely preserved.
        CHECK(frame->slots[0] == 0xAA000000ULL + depth);
        CHECK(frame->slots[1] == 0xBB000000ULL + depth);
        CHECK(frame->slots[2] == 0xCC000000ULL + depth);

        bronze_gc_frame_pop();
    };

    recurse(kMaxDepth);
    CHECK(bronze_tls_block_addr()->frame_top == nullptr);
}

TEST_CASE("shadow stack overflow GC traversal and object forwarding") {
    ShadowStackTestGuard guard;

    setShadowStackCapacityForTesting(4);

    // Frame 1: on shadow stack (1 slot + 2 header = 3 words <= 4 words).
    bronze_gc_frame* f1 = bronze_gc_frame_push(2);
    REQUIRE(f1 != nullptr);
    CHECK(isShadowStackFrame(f1));
    Value s1 = rtMakeString("shadow_str");
    f1->slots[0] = s1.rawBits();
    Value o1 = makeTaggedObject(100.0);
    f1->slots[1] = o1.rawBits();

    // Frame 2: overflow frame.
    bronze_gc_frame* f2 = bronze_gc_frame_push(2);
    REQUIRE(f2 != nullptr);
    CHECK_FALSE(isShadowStackFrame(f2));
    Value s2 = rtMakeString("overflow_str_2");
    f2->slots[0] = s2.rawBits();
    Value o2 = makeTaggedObject(200.0);
    f2->slots[1] = o2.rawBits();

    // Frame 3: nested overflow frame.
    bronze_gc_frame* f3 = bronze_gc_frame_push(2);
    REQUIRE(f3 != nullptr);
    CHECK_FALSE(isShadowStackFrame(f3));
    CHECK(f3 != f2);
    Value s3 = rtMakeString("overflow_str_3");
    f3->slots[0] = s3.rawBits();
    Value o3 = makeTaggedObject(300.0);
    f3->slots[1] = o3.rawBits();

    // Trigger GC collection while multiple frames are in overflow.
    rtHeap().collect();

    // Verify all objects in all frames (both shadow stack and overflow) were forwarded and survived.
    Value s1_post(f1->slots[0]);
    CHECK(s1_post.isString());
    CHECK(rtUtf8Chars(s1_post.asString<StringHeader>()) == "shadow_str");
    CHECK(getTaggedValue(Value(f1->slots[1])) == 100.0);

    Value s2_post(f2->slots[0]);
    CHECK(s2_post.isString());
    CHECK(rtUtf8Chars(s2_post.asString<StringHeader>()) == "overflow_str_2");
    CHECK(getTaggedValue(Value(f2->slots[1])) == 200.0);

    Value s3_post(f3->slots[0]);
    CHECK(s3_post.isString());
    CHECK(rtUtf8Chars(s3_post.asString<StringHeader>()) == "overflow_str_3");
    CHECK(getTaggedValue(Value(f3->slots[1])) == 300.0);

    // Trigger a second collection to verify idempotence and multi-collection survival.
    rtHeap().collect();

    CHECK(rtUtf8Chars(Value(f1->slots[0]).asString<StringHeader>()) == "shadow_str");
    CHECK(getTaggedValue(Value(f1->slots[1])) == 100.0);
    CHECK(rtUtf8Chars(Value(f2->slots[0]).asString<StringHeader>()) == "overflow_str_2");
    CHECK(getTaggedValue(Value(f2->slots[1])) == 200.0);
    CHECK(rtUtf8Chars(Value(f3->slots[0]).asString<StringHeader>()) == "overflow_str_3");
    CHECK(getTaggedValue(Value(f3->slots[1])) == 300.0);

    // Unwind frames in LIFO order.
    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == f2);

    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == f1);

    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == nullptr);
}

TEST_CASE("shadow stack huge frame overflow and nested calls") {
    ShadowStackTestGuard guard;

    constexpr size_t kCapacityWords = (64 * 1024 * 1024) / sizeof(uint64_t);
    const uint32_t hugeCount = static_cast<uint32_t>(kCapacityWords + 10);

    bronze_gc_frame* hugeFrame = bronze_gc_frame_push(hugeCount);
    REQUIRE(hugeFrame != nullptr);
    CHECK_FALSE(isShadowStackFrame(hugeFrame));

    CHECK(bronze_exception_pending() != 0);
    bronze_exception_take();

    hugeFrame->slots[0] = 0xDEADBEEFULL;
    hugeFrame->slots[hugeCount - 1] = 0xCAFEBABEAFFEULL;

    // Push a nested frame while the huge overflow frame is active.
    bronze_gc_frame* nestedFrame = bronze_gc_frame_push(3);
    REQUIRE(nestedFrame != nullptr);
    CHECK(nestedFrame != hugeFrame);
    CHECK(bronze_tls_block_addr()->frame_top == nestedFrame);
    CHECK(nestedFrame->prev == hugeFrame);

    nestedFrame->slots[0] = 0x111;
    nestedFrame->slots[1] = 0x222;
    nestedFrame->slots[2] = 0x333;

    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == hugeFrame);
    CHECK(hugeFrame->slots[0] == 0xDEADBEEFULL);
    CHECK(hugeFrame->slots[hugeCount - 1] == 0xCAFEBABEAFFEULL);

    bronze_gc_frame_pop();
    CHECK(bronze_tls_block_addr()->frame_top == nullptr);
}
