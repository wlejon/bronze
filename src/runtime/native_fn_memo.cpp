#include "runtime/native_fn_memo.h"
#include "runtime/tls_block.h"

#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/profile.h"

namespace bronze::runtime {

namespace {

// splitmix64's finalizer, for the same reason elem_ic.cpp uses it: both keys
// here have dead low bits — a code pointer is 16-byte aligned function-entry
// text and a key index is a small integer — and a mix that leaves them dead
// funnels every entry into a handful of buckets.
uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// ---- the code-pointer memo ------------------------------------------------

struct CodeMemoEntry {
    bronze_fn_code code = nullptr;
    uint32_t index = 0;
};

// 512 entries: the runtime interns a few hundred natives across every builtin
// family it has, so this holds the whole working set and stays in L1. Direct
// mapped — a collision costs a refill, never a wrong answer, because the
// answer is validated against the vector by code-pointer identity.
constexpr uint32_t kCodeMemoEntries = 512;
thread_local CodeMemoEntry g_codeMemo[kCodeMemoEntries];

// ---- the (kind, key) member memo -----------------------------------------

struct MemberMemoEntry {
    uint32_t keyIndex = UINT32_MAX;
    uint16_t kind = 0;
    // Into the interned-native vector, with the code pointer beside it so the
    // read is validated the same way the code memo's is.
    uint32_t index = 0;
    bronze_fn_code code = nullptr;
};

// 256: four collection kinds times the dozen members three.js's renderer
// actually reads, with room to spare.
constexpr uint32_t kMemberMemoEntries = 256;
thread_local MemberMemoEntry g_memberMemo[kMemberMemoEntries];

}  // namespace

bool rtNativeMemoEnabled() noexcept {
    return rtTls()->fn_singleton_cache_enabled != 0;
}

Value rtNativeSingleton(bronze_fn_code code, uint32_t arity) {
    if (!code || !rtNativeMemoEnabled()) {
        return Value(bronze_function_singleton(code, arity, /*length=*/0,
                                               BRONZE_ABI_FN_NAME_NONE,
                                               BRONZE_ABI_FN_FLAGS_ORDINARY |
                                                   BRONZE_ABI_FN_FLAG_NATIVE,
                                               /*slotCell=*/nullptr));
    }
    CodeMemoEntry& e =
        g_codeMemo[static_cast<uint32_t>(mix64(reinterpret_cast<uintptr_t>(code))) &
                   (kCodeMemoEntries - 1)];
    if (e.code == code) {
        // Validated against the vector, not trusted: a module unload erases
        // entries and renumbers the rest, and an index that now names another
        // code pointer answers undefined here rather than the wrong function.
        const Value hit = rtFunctionSingletonAt(e.index, code);
        if (!hit.isUndefined()) return hit;
    }
    const Value made = Value(bronze_function_singleton(code, arity, /*length=*/0,
                                                       BRONZE_ABI_FN_NAME_NONE,
                                                       BRONZE_ABI_FN_FLAGS_ORDINARY |
                                                           BRONZE_ABI_FN_FLAG_NATIVE,
                                                       /*slotCell=*/nullptr));
    if (const uint32_t idx = rtFunctionSingletonIndexOf(code); idx != UINT32_MAX) {
        e.code = code;
        e.index = idx;
    }
    return made;
}

Value rtNativeMemberProbe(uint16_t kind, uint32_t keyIndex) {
    if (keyIndex == UINT32_MAX || !rtNativeMemoEnabled()) return Value::fromUndefined();
    const MemberMemoEntry& e =
        g_memberMemo[static_cast<uint32_t>(mix64((static_cast<uint64_t>(kind) << 32) | keyIndex)) &
                     (kMemberMemoEntries - 1)];
    if (e.keyIndex != keyIndex || e.kind != kind || !e.code) return Value::fromUndefined();
    return rtFunctionSingletonAt(e.index, e.code);
}

void rtNativeMemberFill(uint16_t kind, uint32_t keyIndex, Value resolved) {
    if (keyIndex == UINT32_MAX || !rtNativeMemoEnabled()) return;
    // Only a plain interned native is memoizable. A member whose answer is
    // computed from the receiver (`size`) is not one, and neither is a member
    // that does not exist — an absent name here is a DIAGNOSED name, and the
    // diagnostic has to keep running.
    if (!resolved.isObject()) return;
    auto* hdr = resolved.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Function) return;
    auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
    if (!fn->code) return;
    const uint32_t idx = rtFunctionSingletonIndexOf(fn->code);
    if (idx == UINT32_MAX) return;
    // The value the vector holds at that index must BE this value: an
    // interned native is the vector's entry, and anything else reaching here
    // (a bound function, a closure that happens to share a code pointer) is
    // refused rather than memoized under a name it does not own.
    if (rtFunctionSingletonAt(idx, fn->code).rawBits() != resolved.rawBits()) return;
    MemberMemoEntry& e =
        g_memberMemo[static_cast<uint32_t>(mix64((static_cast<uint64_t>(kind) << 32) | keyIndex)) &
                     (kMemberMemoEntries - 1)];
    e.keyIndex = keyIndex;
    e.kind = kind;
    e.index = idx;
    e.code = fn->code;
}

}  // namespace bronze::runtime
