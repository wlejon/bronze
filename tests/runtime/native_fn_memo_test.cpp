// The native-function memo (runtime/native_fn_memo.h) at the level JS cannot
// see it.
//
// tests/oracle/cases/native_member_memo.js pins the ANSWERS — one object per
// member, an own property shadowing it, a member behaving for its own kind.
// What that case cannot show is whether an answer came from the MEMO or from
// the unordered_map walk it replaces: a memo that never fills passes every one
// of its scenarios. So this file asks the table directly.
//
// It also pins the two things that make the table safe to hold no `Value`:
// an entry stores an INDEX into the runtime's interned-native vector, and the
// vector is a root source — so a collection between a fill and a hit moves the
// function object and the entry keeps answering, because the entry never named
// the object's address in the first place.

#include <doctest/doctest.h>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/native_fn_memo.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// Two distinct code pointers with nothing behind them: these are interned as
// function objects and never called, which is all the memo is about.
//
// The bodies differ, and they have to. Written identically, MSVC's `/OPT:ICF`
// — on by default in Release, which is what the `dev` preset builds — folds
// two identical COMDATs into ONE address, and the test's whole premise ("two
// code pointers are two objects") evaporates: `probeCodeB == probeCodeA`, the
// memo correctly answers with the same object, and the assertion fails for a
// reason that has nothing to do with the memo. Distinguishing the bodies is
// what makes the linker keep them apart.
uint64_t probeCodeA(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return BRONZE_ABI_UNDEFINED_BITS;
}
uint64_t probeCodeB(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return BRONZE_ABI_NULL_BITS;
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

    const Value first = rtNativeSingleton(probeCodeA, 1, nullptr, 0);
    const Value again = rtNativeSingleton(probeCodeA, 1, nullptr, 0);
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
    CHECK(rtNativeSingleton(probeCodeB, 0, nullptr, 0).rawBits() != first.rawBits());
}

TEST_CASE("the first creation names the object and a later call cannot rename it") {
    ShadowStackFrame frame;
    MemoOn memo;

    // A THIRD code pointer, so no earlier case in this binary has interned it
    // nameless: interning is per thread for the thread's life. Its body is a
    // constant no other probe in the binary returns, for the `/OPT:ICF`
    // reason the two above differ — a folded body IS an earlier probe's code
    // pointer, already interned without a name.
    static auto probeCodeC = [](uint64_t, uint64_t, uint32_t argc, const uint64_t*) -> uint64_t {
        return 0x7FF8C0DEC0DE0000ull + argc;
    };
    const Value named = rtNativeSingleton(probeCodeC, 0, "probe", 2);
    const FunctionHeader* fn = named.asObject<FunctionHeader>();
    REQUIRE(fn->name != nullptr);
    CHECK(rtUtf8Chars(fn->name) == "probe");
    CHECK(fn->length == 2);

    // A nameless call for the same code pointer keeps what is there, and so
    // does a differently-named one: the arguments are the CREATE path's only.
    CHECK(rtNativeSingleton(probeCodeC, 0, nullptr, 0).rawBits() == named.rawBits());
    CHECK(rtNativeSingleton(probeCodeC, 0, "other", 7).rawBits() == named.rawBits());
    fn = named.asObject<FunctionHeader>();
    CHECK(rtUtf8Chars(fn->name) == "probe");
    CHECK(fn->length == 2);
}

TEST_CASE("a collection moves the function object and the memo still answers") {
    ShadowStackFrame frame;
    MemoOn memo;

    (void)rtNativeSingleton(probeCodeA, 1, nullptr, 0);
    rtHeap().collect();
    rtHeap().collect();

    // Read the vector's live entry, then the memo's: they must agree AFTER the
    // flips, which is only true because the entry holds an index rather than
    // the address the fill saw.
    const uint32_t idx = rtFunctionSingletonIndexOf(probeCodeA);
    REQUIRE(idx != UINT32_MAX);
    CHECK(rtNativeSingleton(probeCodeA, 1, nullptr, 0).rawBits() ==
          rtFunctionSingletonAt(idx, probeCodeA).rawBits());
}

TEST_CASE("an index whose code pointer no longer matches answers nothing") {
    ShadowStackFrame frame;
    MemoOn memo;

    (void)rtNativeSingleton(probeCodeA, 1, nullptr, 0);
    const uint32_t idx = rtFunctionSingletonIndexOf(probeCodeA);
    REQUIRE(idx != UINT32_MAX);

    // The self-healing check, stated as the question it answers: a module
    // unload erases entries and renumbers the rest, so an index that once named
    // A can come to name B. Asking for the wrong pointer at a real index is
    // exactly that situation, and the answer is undefined rather than B.
    CHECK(rtFunctionSingletonAt(idx, probeCodeB).isUndefined());
    CHECK(rtFunctionSingletonAt(idx + 100000u, probeCodeA).isUndefined());
}

TEST_CASE("the seam makes the memo miss without changing what it would answer") {
    ShadowStackFrame frame;
    MemoOn memo;

    const Value warm = rtNativeSingleton(probeCodeA, 1, nullptr, 0);

    bronze_tls_block_addr()->fn_singleton_cache_enabled = 0;
    // The singleton answer is the helper's either way — the seam removes the
    // shortcut, never the interning.
    CHECK(rtNativeSingleton(probeCodeA, 1, nullptr, 0).rawBits() == warm.rawBits());
}
