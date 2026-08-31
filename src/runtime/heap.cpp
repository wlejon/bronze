// The memory the heap is made of and the paths that hand it out: the OS
// reservation, the commit growth, the bump allocators for both semispaces, the
// per-thread seam settling one Heap construction does on its way up, and the
// non-moving arena that lives beside the movable heap. What COPIES the live set
// out of this memory is heap_collect.cpp; what audits it afterwards is
// heap_verify.cpp.

#include "runtime/heap.h"

#include "abi/bronze_abi.h"
#include "runtime/elem_ic.h"
#include "runtime/heap_internal.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace bronze {

// The inline-allocation window (see the bronze_tls_block comment in
// bronze_abi.h): generated code's `new` fast path bump-allocates plain
// instances from [cursor, limit) and never collects — refill_inline_lab below
// is the only producer, and Heap::collect zeroes both words because the
// window points into the semispace a collection abandons. 0/0 is the dormant
// state: the unsigned headroom subtraction is then 0 and every construct site
// falls back to bronze_construct. The words themselves live in the calling
// thread's bronze_tls_block (tls_block.cpp), which is what keeps this heap's
// window — and every other word generated code shares with the runtime — the
// property of the thread that owns this heap.

namespace heap_internal {

GcLogStats g_gcLog;

void dumpGcLog() {
    if (!g_gcLog.enabled) return;
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g_gcLog.start)
                        .count();
    std::fprintf(stderr, "\n=== Bronze GC Log (BRONZE_GC_LOG=1) ===\n");
    std::fprintf(stderr, "collections      : %llu\n",
                 static_cast<unsigned long long>(g_gcLog.collections));
    std::fprintf(stderr, "allocations      : %llu (%.2f MB)\n",
                 static_cast<unsigned long long>(g_gcLog.alloc_count),
                 g_gcLog.alloc_bytes / (1024.0 * 1024.0));
    std::fprintf(stderr, "bytes copied     : %.2f MB\n",
                 g_gcLog.copied_bytes / (1024.0 * 1024.0));
    std::fprintf(stderr, "time in collect(): %.3f ms\n", g_gcLog.gc_nanos / 1e6);
    std::fprintf(stderr, "process wall     : %.3f ms\n", total_ns / 1e6);
    std::fflush(stderr);
}

}  // namespace heap_internal

constexpr uintptr_t kMaxLowAddressLimit = 1ULL << 47;

void* VirtualMemory::reserve(size_t bytes) {
#ifdef _WIN32
    void* ptr = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!ptr) {
        throw std::bad_alloc();
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr >= kMaxLowAddressLimit) {
        VirtualFree(ptr, 0, MEM_RELEASE);
        throw std::runtime_error("Heap VirtualAlloc reserved address exceeds 47-bit range");
    }
    return ptr;
#else
    void* ptr = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        throw std::bad_alloc();
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr >= kMaxLowAddressLimit) {
        munmap(ptr, bytes);
        throw std::runtime_error("Heap mmap reserved address exceeds 47-bit range");
    }
    return ptr;
#endif
}

bool VirtualMemory::commit(void* ptr, size_t bytes) {
#ifdef _WIN32
    return VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(ptr, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

void VirtualMemory::decommit(void* ptr, size_t bytes) {
#ifdef _WIN32
    VirtualFree(ptr, bytes, MEM_DECOMMIT);
#else
    mprotect(ptr, bytes, PROT_NONE);
#endif
}

void VirtualMemory::release(void* ptr, size_t bytes) {
#ifdef _WIN32
    (void)bytes;
    if (ptr) {
        VirtualFree(ptr, 0, MEM_RELEASE);
    }
#else
    if (ptr && ptr != MAP_FAILED) {
        munmap(ptr, bytes);
    }
#endif
}

Heap::Heap(size_t reserve_bytes, size_t initial_commit_bytes)
    : reserved_bytes_(reserve_bytes) {
    reserved_base_ = VirtualMemory::reserve(reserved_bytes_);
    semispace_size_ = reserved_bytes_ / 2;

    from_space_.base = static_cast<uint8_t*>(reserved_base_);
    from_space_.size = semispace_size_;
    from_space_.committed_bytes = 0;
    from_space_.bump_ptr = from_space_.base;

    to_space_.base = static_cast<uint8_t*>(reserved_base_) + semispace_size_;
    to_space_.size = semispace_size_;
    to_space_.committed_bytes = 0;
    to_space_.bump_ptr = to_space_.base;

    if (initial_commit_bytes > 0) {
        size_t commit_target = std::min(initial_commit_bytes, semispace_size_);
        ensure_commit(from_space_, commit_target);
    }
    gc_threshold_bytes_ = std::min(semispace_size_, static_cast<size_t>(16 * 1024 * 1024));

    const char* env_stress = std::getenv("BRONZE_GC_STRESS");
    if (env_stress && (std::strcmp(env_stress, "1") == 0 ||
                       std::strcmp(env_stress, "true") == 0 ||
                       std::strcmp(env_stress, "ON") == 0)) {
        gc_stress_mode_ = true;
    }

    const char* env_poison = std::getenv("BRONZE_GC_POISON");
    if (env_poison && std::strcmp(env_poison, "1") == 0) {
        gc_poison_mode_ = true;
    }

    const char* env_verify = std::getenv("BRONZE_HEAP_VERIFY");
    if (env_verify && std::strcmp(env_verify, "1") == 0) {
        gc_verify_mode_ = true;
    }

    const char* env_no_inline = std::getenv("BRONZE_NO_INLINE_ALLOC");
    if (env_no_inline && std::strcmp(env_no_inline, "1") == 0) {
        inline_lab_enabled_ = false;
    }

    // The inline fast-path enable flags default to 1 in the TLS block; the
    // env overrides land here because this constructor runs exactly once per
    // thread that touches the runtime (rtHeap's first touch), before any
    // generated code on that thread can read a flag through a helper.
    bronze_tls_block* tls = bronze_tls_block_addr();
    const char* env_no_call = std::getenv("BRONZE_NO_INLINE_CALL");
    if (env_no_call && std::strcmp(env_no_call, "1") == 0) {
        tls->inline_call_enabled = 0;
    }

    const char* env_no_array_ic = std::getenv("BRONZE_NO_ARRAY_METHOD_IC");
    if (env_no_array_ic && std::strcmp(env_no_array_ic, "1") == 0) {
        tls->array_method_ic_enabled = 0;
    }

    const char* env_no_overflow_set = std::getenv("BRONZE_NO_INLINE_OVERFLOW_SET");
    if (env_no_overflow_set && std::strcmp(env_no_overflow_set, "1") == 0) {
        tls->inline_overflow_set_enabled = 0;
    }

    const char* env_no_accessor = std::getenv("BRONZE_NO_INLINE_ACCESSOR");
    if (env_no_accessor && std::strcmp(env_no_accessor, "1") == 0) {
        tls->inline_accessor_enabled = 0;
    }

    const char* env_no_poly = std::getenv("BRONZE_NO_POLY_IC");
    if (env_no_poly && std::strcmp(env_no_poly, "1") == 0) {
        tls->poly_ic_enabled = 0;
    }

    const char* env_no_neg = std::getenv("BRONZE_NO_NEG_IC");
    if (env_no_neg && std::strcmp(env_no_neg, "1") == 0) {
        tls->negative_ic_enabled = 0;
    }

    const char* env_no_elem = std::getenv("BRONZE_NO_ELEM_IC");
    if (env_no_elem && std::strcmp(env_no_elem, "1") == 0) {
        tls->elem_ic_enabled = 0;
    }

    // BRONZE_NO_ELEM_SET_IC, read by elem_ic.cpp because its flag is not in the
    // ABI block, but read HERE so every seam is settled at one first touch.
    runtime::elemSetCacheReadSeam();

    // BRONZE_NO_FN_STATICS_IC, read here for the same reason: the flag lives in
    // rt_prop_function.cpp rather than in the ABI's TLS block, and every seam is
    // settled at one first touch.
    runtime::fnStaticsIcReadSeam();

    const char* env_no_callout = std::getenv("BRONZE_NO_DIRECT_CALLOUT");
    if (env_no_callout && std::strcmp(env_no_callout, "1") == 0) {
        tls->direct_callout_enabled = 0;
    }

    const char* env_no_elem_absent = std::getenv("BRONZE_NO_ELEM_ABSENT");
    if (env_no_elem_absent && std::strcmp(env_no_elem_absent, "1") == 0) {
        tls->elem_absent_enabled = 0;
    }

    // The string-key identity latch, latch-side: with this off no fill or hit
    // ever writes a non-zero key_ident, so the inline string arm can only
    // miss into the helper it always took (elem_ic.h).
    const char* env_no_elem_key = std::getenv("BRONZE_NO_ELEM_KEY_IC");
    if (env_no_elem_key && std::strcmp(env_no_elem_key, "1") == 0) {
        tls->elem_key_ic_enabled = 0;
    }

    // The undefined-vs-number relational arm: with this off, a compare whose
    // operand is `undefined` keeps the bronze_rel_* helper it always took.
    const char* env_no_undef_rel = std::getenv("BRONZE_NO_UNDEF_REL");
    if (env_no_undef_rel && std::strcmp(env_no_undef_rel, "1") == 0) {
        tls->undef_rel_enabled = 0;
    }

    // Array.prototype.sort's hoisted-roots merge engine: with this off the
    // sort keeps the per-comparison Rooted churn it always had (one binary
    // A/B; builtin_array_sort.cpp).
    const char* env_no_sort_fast = std::getenv("BRONZE_NO_SORT_FAST");
    if (env_no_sort_fast && std::strcmp(env_no_sort_fast, "1") == 0) {
        tls->sort_fast_enabled = 0;
    }

    // The allocation-free Map/WeakMap lookup probe: with this off every
    // `get`/`has` runs the full rooted prologue it always did (map.cpp,
    // builtin_weak_map.cpp).
    const char* env_no_map_fast = std::getenv("BRONZE_NO_MAP_FAST");
    if (env_no_map_fast && std::strcmp(env_no_map_fast, "1") == 0) {
        tls->map_fast_enabled = 0;
    }

    // %TypedArray%.prototype.set's number-elements fast loop over a plain
    // array source: with this off every element keeps its rooted spec-shaped
    // iteration (builtin_typed_array_methods.cpp).
    const char* env_no_ta_set = std::getenv("BRONZE_NO_TA_SET_FAST");
    if (env_no_ta_set && std::strcmp(env_no_ta_set, "1") == 0) {
        tls->ta_set_fast_enabled = 0;
    }

    // The inline truthiness arms for bool/undefined/null/object operands:
    // with this off only the pre-existing number arm stays inline and every
    // other operand keeps the bronze_unbox_bool helper (llvm_ops.cpp).
    const char* env_no_truthy = std::getenv("BRONZE_NO_TRUTHY_INLINE");
    if (env_no_truthy && std::strcmp(env_no_truthy, "1") == 0) {
        tls->truthy_inline_enabled = 0;
    }

    const char* env_no_fn_singleton = std::getenv("BRONZE_NO_FN_SINGLETON_CACHE");
    if (env_no_fn_singleton && std::strcmp(env_no_fn_singleton, "1") == 0) {
        tls->fn_singleton_cache_enabled = 0;
    }

    const char* env_no_iter_fast = std::getenv("BRONZE_NO_ITER_FAST");
    if (env_no_iter_fast && std::strcmp(env_no_iter_fast, "1") == 0) {
        tls->iter_fast_enabled = 0;
    }

    const char* env_no_inline_roots = std::getenv("BRONZE_NO_INLINE_ROOTS");
    if (env_no_inline_roots && std::strcmp(env_no_inline_roots, "1") == 0) {
        tls->inline_roots_enabled = 0;
    }

    const char* env_no_strict_eq = std::getenv("BRONZE_NO_STRICT_EQ_INLINE");
    if (env_no_strict_eq && std::strcmp(env_no_strict_eq, "1") == 0) {
        tls->strict_eq_inline_enabled = 0;
    }

    // Two ways to lower the inline elem probe, and the second is not a
    // convenience: with the TABLE off nothing is ever installed, so an inline
    // probe could only miss, and charging chunk 3's A/B for a probe that
    // cannot hit would read as a regression in a mechanism that is not there.
    const char* env_no_elem_inline = std::getenv("BRONZE_NO_ELEM_INLINE");
    if ((env_no_elem_inline && std::strcmp(env_no_elem_inline, "1") == 0) ||
        tls->elem_ic_enabled == 0) {
        tls->elem_inline_enabled = 0;
    }

    const char* env_no_method_call_ic = std::getenv("BRONZE_NO_METHOD_CALL_IC");
    if (!env_no_method_call_ic) env_no_method_call_ic = std::getenv("BRONZE_NO_CALL_IC");
    if (env_no_method_call_ic && std::strcmp(env_no_method_call_ic, "1") == 0) {
        tls->method_call_ic_enabled = 0;
    }

    // Narrower than the switch above: the method IC stays, but latches only
    // the env-free direct entries it originally could — rt_state.h's
    // rtSetEnvMethodIcEnabled says what the two gated forms are.
    const char* env_no_env_method_ic = std::getenv("BRONZE_NO_ENV_METHOD_IC");
    if (env_no_env_method_ic && std::strcmp(env_no_env_method_ic, "1") == 0) {
        runtime::rtSetEnvMethodIcEnabled(false);
    }

    // Narrower still: the method IC keeps every plain-receiver form, but never
    // latches the exotic-receiver (Array/collection) entries —
    // rt_state.h's rtSetExoticMethodIcEnabled says why latch-side is enough.
    const char* env_no_exotic_method_ic = std::getenv("BRONZE_NO_EXOTIC_METHOD_IC");
    if (env_no_exotic_method_ic && std::strcmp(env_no_exotic_method_ic, "1") == 0) {
        runtime::rtSetExoticMethodIcEnabled(false);
    }

    // And narrower again: way-0 latching keeps every form, but a displaced
    // plain-direct entry is dropped instead of moved to way 1
    // (rt_state.h's rtSetPolyMethodIcEnabled).
    const char* env_no_poly_method_ic = std::getenv("BRONZE_NO_POLY_METHOD_IC");
    if (env_no_poly_method_ic && std::strcmp(env_no_poly_method_ic, "1") == 0) {
        runtime::rtSetPolyMethodIcEnabled(false);
    }

    // Shape-census mode (BRONZE_SHAPE_CENSUS=1, runtime/shape_census.h):
    // every latch the runtime can reach through a TLS word goes down, so all
    // property traffic keeps missing into the helpers that record it. The
    // remaining latches — the property-IC fills, the absent install, the
    // static publish, the family stamp, the method-IC latch — consult
    // censusFillsSuppressed() (or method_call_ic_enabled below) at their own
    // sites.
    const char* env_census = std::getenv("BRONZE_SHAPE_CENSUS");
    if (env_census && std::strcmp(env_census, "1") == 0) {
        tls->elem_ic_enabled = 0;
        tls->elem_inline_enabled = 0;
        tls->elem_key_ic_enabled = 0;
        tls->elem_absent_enabled = 0;
        tls->array_method_ic_enabled = 0;
        tls->method_call_ic_enabled = 0;
    }

    // The computed-read cache's table address, published where the seam that
    // gates reading it is set, so a thread never has one without the other.
    runtime::elemCachePublish();

    // The ident sweep runs inside every collection pause of THIS heap: an
    // address is reused only across a collection, so clearing every
    // movable-heap ident before the mutator resumes is what makes the inline
    // string arm's single-compare guard sound (elem_ic.h). The bounds are the
    // whole reservation — both semispaces — so a stale ident can never
    // straddle the swap.
    {
        const uintptr_t ident_lo = reinterpret_cast<uintptr_t>(reserved_base_);
        const uintptr_t ident_hi = ident_lo + reserved_bytes_;
        add_post_collection_hook(
            [ident_lo, ident_hi] { runtime::elemCacheSweepIdent(ident_lo, ident_hi); });
        // And a full wipe now: the thread's table may carry idents from an
        // earlier Heap (unit tests construct them directly), and a fresh
        // reservation can land where an old one was. The runtime's own heap
        // is a leaked per-thread singleton, so for programs this wipes an
        // empty table exactly once.
        runtime::elemCacheSweepIdent(0, UINTPTR_MAX);
    }

    const char* env_log = std::getenv("BRONZE_GC_LOG");
    if (env_log && std::strcmp(env_log, "1") == 0 && !g_gcLog.enabled) {
        g_gcLog.enabled = true;
        g_gcLog.start = std::chrono::steady_clock::now();
        std::atexit(dumpGcLog);
    }
}

Heap::~Heap() {
    // Retract the inline-allocation window if this heap published it: the
    // memory under it is released on the next line, and this thread's TLS
    // block outlives a Heap (in tests) that need not be the thread's last.
    bronze_tls_block* tls = bronze_tls_block_addr();
    if (tls->alloc_cursor >= reinterpret_cast<uint64_t>(reserved_base_) &&
        tls->alloc_cursor < reinterpret_cast<uint64_t>(reserved_base_) + reserved_bytes_) {
        tls->alloc_cursor = 0;
        tls->alloc_limit = 0;
    }
    if (reserved_base_) {
        VirtualMemory::release(reserved_base_, reserved_bytes_);
        reserved_base_ = nullptr;
    }
}

bool Heap::ensure_commit(Semispace& space, size_t required_bytes) {
    if (required_bytes <= space.committed_bytes) {
        return true;
    }
    if (required_bytes > space.size) {
        return false;
    }

    constexpr size_t kMinStep = 64 * 1024;
    size_t growth = std::max(kMinStep, space.committed_bytes);
    size_t target_commit = std::max(required_bytes, space.committed_bytes + growth);
    target_commit = (target_commit + kMinStep - 1) & ~(kMinStep - 1);
    target_commit = std::min(target_commit, space.size);
    size_t commit_size = target_commit - space.committed_bytes;
    uint8_t* commit_addr = space.base + space.committed_bytes;

    if (!VirtualMemory::commit(commit_addr, commit_size)) {
        return false;
    }

    space.committed_bytes = target_commit;
    return true;
}

void* Heap::allocate_in_space(Semispace& space, size_t bytes) {
    size_t aligned_bytes = (bytes + 7) & ~static_cast<size_t>(7);
    size_t current_used = space.bump_ptr - space.base;
    size_t needed = current_used + aligned_bytes;

    if (needed > space.size) {
        throw std::bad_alloc();
    }

    if (needed > space.committed_bytes) {
        if (!ensure_commit(space, needed)) {
            throw std::bad_alloc();
        }
    }

    uint8_t* ptr = space.bump_ptr;
    space.bump_ptr += aligned_bytes;
    return ptr;
}

void* Heap::allocate_raw(size_t bytes) {
    if (gc_stress_mode_ && !in_gc_) {
        collect();
    }

    size_t aligned_bytes = (bytes + 7) & ~static_cast<size_t>(7);
    size_t current_used = from_space_.bump_ptr - from_space_.base;
    size_t needed = current_used + aligned_bytes;

    if (!in_gc_ && needed > gc_threshold_bytes_) {
        collect();
        current_used = from_space_.bump_ptr - from_space_.base;
        needed = current_used + aligned_bytes;
    }

    if (needed > from_space_.size || needed > from_space_.committed_bytes) {
        if (!ensure_commit(from_space_, needed)) {
            if (!in_gc_) {
                collect();
                current_used = from_space_.bump_ptr - from_space_.base;
                needed = current_used + aligned_bytes;
                if (!ensure_commit(from_space_, needed)) {
                    throw std::bad_alloc();
                }
            } else {
                throw std::bad_alloc();
            }
        }
    }

    uint8_t* ptr = from_space_.bump_ptr;
    from_space_.bump_ptr += aligned_bytes;
    if (g_gcLog.enabled) {
        g_gcLog.alloc_bytes += aligned_bytes;
        ++g_gcLog.alloc_count;
    }
    return ptr;
}

HeapObjectHeader* Heap::allocate(size_t bytes, Tag tag) {
    if (gc_stress_mode_ && !in_gc_) {
        collect();
    }

    size_t total_bytes = sizeof(HeapObjectHeader) + bytes;
    void* mem = allocate_raw(total_bytes);
    auto* header = static_cast<HeapObjectHeader*>(mem);
    header->tag = static_cast<uint16_t>(tag);
    // A raw zero and deliberately not `HeapKind::Plain`: this word is a heap
    // kind only for a `Tag::Object`, and a String spends it on its encoding
    // bits. The caller that knows which tag it asked for is the one that gets
    // to name what goes in here.
    header->flags = 0;
    header->size = static_cast<uint32_t>((total_bytes + 7) & ~static_cast<size_t>(7));
    return header;
}

void Heap::refill_inline_lab() {
    if (!inline_lab_enabled_) return;
    // Under stress: exactly one plain object, so the inline path runs on the
    // very next `new` — its rooting across the constructor call is what the
    // stress mode exists to shake — and the `new` after that misses back into
    // the helper, whose allocations collect. Without stress: a run long
    // enough that the helper is a rounding error, small enough that a
    // collection abandons nothing worth naming.
    constexpr size_t kLabBytes = 256 * 1024;
    const size_t bytes = gc_stress_mode_ ? BRONZE_ABI_PLAIN_OBJECT_BYTES : kLabBytes;
    // allocate_raw may collect (stress does so every time), which zeroes the
    // window — publishing AFTER it returns is what keeps the two ordered.
    void* run = allocate_raw(bytes);
    bronze_tls_block* tls = bronze_tls_block_addr();
    tls->alloc_cursor = reinterpret_cast<uint64_t>(run);
    tls->alloc_limit = tls->alloc_cursor + bytes;
}

NonMovingArena::NonMovingArena(size_t chunk_size) : chunk_size_(chunk_size) {}

NonMovingArena::~NonMovingArena() {
    for (size_t i = 0; i < chunks_.size(); ++i) {
        VirtualMemory::release(chunks_[i], chunk_capacities_[i]);
    }
}

void NonMovingArena::allocate_new_chunk(size_t min_bytes) {
    size_t capacity = std::max(chunk_size_, min_bytes);
    void* mem = VirtualMemory::reserve(capacity);
    if (!VirtualMemory::commit(mem, capacity)) {
        VirtualMemory::release(mem, capacity);
        throw std::bad_alloc();
    }
    chunks_.push_back(static_cast<uint8_t*>(mem));
    chunk_capacities_.push_back(capacity);
    current_offset_ = 0;
    current_chunk_capacity_ = capacity;
}

void* NonMovingArena::allocate(size_t bytes, size_t alignment) {
    if (alignment == 0) alignment = 8;
    if (chunks_.empty()) {
        allocate_new_chunk(bytes + alignment);
    }

    uint8_t* current = chunks_.back() + current_offset_;
    uintptr_t addr = reinterpret_cast<uintptr_t>(current);
    size_t padding = (alignment - (addr % alignment)) % alignment;

    if (current_offset_ + padding + bytes > current_chunk_capacity_) {
        allocate_new_chunk(bytes + alignment);
        current = chunks_.back();
        addr = reinterpret_cast<uintptr_t>(current);
        padding = (alignment - (addr % alignment)) % alignment;
    }

    current_offset_ += padding + bytes;
    total_allocated_ += padding + bytes;
    return current + padding;
}

}  // namespace bronze
