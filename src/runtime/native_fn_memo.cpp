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

}  // namespace

bool rtNativeMemoEnabled() noexcept {
    return rtTls()->fn_singleton_cache_enabled != 0;
}

namespace {

// The vector's answer, or a fresh object when it has none. The name is
// interned as a key ONLY when an object is made: `bronze_register_key_string`
// is a mutex and a string-keyed map probe, and the memo exists so that a
// repeat interning costs a hash and two loads — paying the key probe on every
// memo collision would put a good part of that bill back.
Value internedOrCreate(bronze_fn_code code, uint32_t arity, const char* name, uint32_t length) {
    if (const uint32_t idx = rtFunctionSingletonIndexOf(code); idx != UINT32_MAX) {
        const Value hit = rtFunctionSingletonAt(idx, code);
        if (!hit.isUndefined()) return hit;
    }
    const uint32_t nameKey = name ? bronze_register_key_string(name) : BRONZE_ABI_FN_NAME_NONE;
    return Value(bronze_function_singleton(code, arity, name ? length : 0, nameKey,
                                           BRONZE_ABI_FN_FLAGS_ORDINARY | BRONZE_ABI_FN_FLAG_NATIVE,
                                           /*slotCell=*/nullptr));
}

}  // namespace

Value rtNativeSingleton(bronze_fn_code code, uint32_t arity, const char* name, uint32_t length) {
    if (!code || !rtNativeMemoEnabled()) return internedOrCreate(code, arity, name, length);
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
    const Value made = internedOrCreate(code, arity, name, length);
    if (const uint32_t idx = rtFunctionSingletonIndexOf(code); idx != UINT32_MAX) {
        e.code = code;
        e.index = idx;
    }
    return made;
}

}  // namespace bronze::runtime
