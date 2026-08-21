// The native-function memos (runtime/native_fn_memo.h) at the level JS cannot
// see them.
//
// tests/oracle/cases/native_member_memo.js pins the ANSWERS — one object per
// member, an own property shadowing it, a member behaving for its own kind.
// What that case cannot show is whether an answer came from the MEMO or from
// the ladder-and-unordered_map walk it replaces: a memo that never fills passes
// every one of its scenarios. So this file asks the tables directly.
//
// It also pins the two things that make the tables safe to hold no `Value`:
// an entry stores an INDEX into the runtime's interned-native vector, and the
// vector is a root source — so a collection between a fill and a hit moves the
// function object and the entry keeps answering, because the entry never named
// the object's address in the first place.

#include <doctest/doctest.h>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/native_fn_memo.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// Two distinct code pointers with nothing behind them: these are interned as
// function objects and never called, which is all the memo is about.
uint64_t probeCodeA(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return BRONZE_ABI_UNDEFINED_BITS;
}
uint64_t probeCodeB(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return BRONZE_ABI_UNDEFINED_BITS;
}

struct MemoOn {
    // `rtHeap()` first: start-up is lazy and it is what reads the BRONZE_NO_*
    // environment, so a flag set before the first touch is put back by it.
    MemoOn() {
        (void)rtHeap();
        saved = bronze_tls_block_addr()->fn_singleton_cache_enabled;
        bronze_tls_block_addr()->fn_singleton_cache_enabled = 1;
    }
    ~MemoOn() { bronze_tls_block_addr()->fn_singleton_cache_enabled = saved; }

private:
    uint64_t saved = 0;
};

}  // namespace

TEST_CASE("the memo answers with the object the helper interned, not another one") {
    ShadowStackFrame frame;
    MemoOn memo;

    const Value first = rtNativeSingleton(probeCodeA, 1);
    const Value again = rtNativeSingleton(probeCodeA, 1);
    CHECK(first.rawBits() == again.rawBits());

    // And it is the same object the helper answers with when asked directly —
    // the memo is a faster route to one answer, not a second answer.
    const Value viaHelper = Value(bronze_function_singleton(
        probeCodeA, 1, /*length=*/0, BRONZE_ABI_FN_NAME_NONE,
        BRONZE_ABI_FN_FLAGS_ORDINARY | BRONZE_ABI_FN_FLAG_NATIVE, /*slotCell=*/nullptr));
    CHECK(viaHelper.rawBits() == first.rawBits());

    // Two code pointers are two objects. A direct-mapped table whose guard was
    // the bucket rather than the code pointer would merge them, and function
    // identity is observable.
    CHECK(rtNativeSingleton(probeCodeB, 0).rawBits() != first.rawBits());
}

TEST_CASE("a collection moves the function object and the memo still answers") {
    ShadowStackFrame frame;
    MemoOn memo;

    (void)rtNativeSingleton(probeCodeA, 1);
    rtHeap().collect();
    rtHeap().collect();

    // Read the vector's live entry, then the memo's: they must agree AFTER the
    // flips, which is only true because the entry holds an index rather than
    // the address the fill saw.
    const uint32_t idx = rtFunctionSingletonIndexOf(probeCodeA);
    REQUIRE(idx != UINT32_MAX);
    CHECK(rtNativeSingleton(probeCodeA, 1).rawBits() ==
          rtFunctionSingletonAt(idx, probeCodeA).rawBits());
}

TEST_CASE("an index whose code pointer no longer matches answers nothing") {
    ShadowStackFrame frame;
    MemoOn memo;

    (void)rtNativeSingleton(probeCodeA, 1);
    const uint32_t idx = rtFunctionSingletonIndexOf(probeCodeA);
    REQUIRE(idx != UINT32_MAX);

    // The self-healing check, stated as the question it answers: a module
    // unload erases entries and renumbers the rest, so an index that once named
    // A can come to name B. Asking for the wrong pointer at a real index is
    // exactly that situation, and the answer is undefined rather than B.
    CHECK(rtFunctionSingletonAt(idx, probeCodeB).isUndefined());
    CHECK(rtFunctionSingletonAt(idx + 100000u, probeCodeA).isUndefined());
}

TEST_CASE("the member memo is keyed on the kind as well as the key") {
    ShadowStackFrame frame;
    MemoOn memo;

    // Filled through the read path rather than by hand, because what is being
    // pinned is that the read path fills it.
    Rooted<Value> map{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
    Rooted<Value> set{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};
    const uint32_t getKey = bronze_register_key_string("get");
    const uint32_t hasKey = bronze_register_key_string("has");

    for (int i = 0; i < 4; ++i) {
        (void)bronze_prop_get(map.get().rawBits(), getKey, nullptr);
        (void)bronze_prop_get(map.get().rawBits(), hasKey, nullptr);
        (void)bronze_prop_get(set.get().rawBits(), hasKey, nullptr);
    }

    const Value mapGet = rtNativeMemberProbe(MapHeader::kMapFlags, getKey);
    CHECK_FALSE(mapGet.isUndefined());
    // A Set has no `get`, so no read ever filled that pair — and the Map's
    // entry must not answer for it.
    CHECK(rtNativeMemberProbe(MapHeader::kSetFlags, getKey).isUndefined());
}

TEST_CASE("the member memo refuses a value that is not an interned native") {
    ShadowStackFrame frame;
    MemoOn memo;

    const uint32_t key = bronze_register_key_string("notANative");
    rtNativeMemberFill(MapHeader::kMapFlags, key, Value::fromDouble(2.0));
    CHECK(rtNativeMemberProbe(MapHeader::kMapFlags, key).isUndefined());

    // A plain object is not one either, and neither is `undefined` — `size`
    // reaches the fill as a number and an absent member never reaches it at
    // all, which is what keeps a diagnosed name diagnosed.
    Rooted<Value> obj{Value(bronze_create_object())};
    rtNativeMemberFill(MapHeader::kMapFlags, key, obj.get());
    CHECK(rtNativeMemberProbe(MapHeader::kMapFlags, key).isUndefined());
}

TEST_CASE("the seam makes both memos miss without changing what they would answer") {
    ShadowStackFrame frame;
    MemoOn memo;

    const Value warm = rtNativeSingleton(probeCodeA, 1);
    const uint32_t key = bronze_register_key_string("size");

    bronze_tls_block_addr()->fn_singleton_cache_enabled = 0;
    CHECK(rtNativeMemberProbe(MapHeader::kMapFlags, key).isUndefined());
    // The singleton answer is the helper's either way — the seam removes the
    // shortcut, never the interning.
    CHECK(rtNativeSingleton(probeCodeA, 1).rawBits() == warm.rawBits());
}
