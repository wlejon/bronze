#define _CRT_SECURE_NO_WARNINGS

#include "runtime/heap.h"

#include "abi/bronze_abi.h"
#include "runtime/elem_ic.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
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

// Measurement, not policy: BRONZE_GC_LOG=1 prints at exit how much of a run
// the collector actually was — collections, bytes copied vs bytes allocated,
// and wall time inside collect(). It exists because "allocation-heavy loop"
// names two different bills (copying survivors, and the per-object allocation
// path itself) and only a number says which one a benchmark is paying.
namespace {
struct GcLogStats {
    bool enabled{false};
    uint64_t alloc_bytes{0};
    uint64_t alloc_count{0};
    uint64_t collections{0};
    uint64_t copied_bytes{0};
    uint64_t gc_nanos{0};
    std::chrono::steady_clock::time_point start;
};
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
}  // namespace

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

    const char* env_no_callout = std::getenv("BRONZE_NO_DIRECT_CALLOUT");
    if (env_no_callout && std::strcmp(env_no_callout, "1") == 0) {
        tls->direct_callout_enabled = 0;
    }

    const char* env_no_elem_absent = std::getenv("BRONZE_NO_ELEM_ABSENT");
    if (env_no_elem_absent && std::strcmp(env_no_elem_absent, "1") == 0) {
        tls->elem_absent_enabled = 0;
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

    // The computed-read cache's table address, published where the seam that
    // gates reading it is set, so a thread never has one without the other.
    runtime::elemCachePublish();

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

    constexpr size_t kPageStep = 64 * 1024;
    size_t target_commit = (required_bytes + kPageStep - 1) & ~(kPageStep - 1);
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
    constexpr size_t kLabBytes = 64 * 1024;
    const size_t bytes = gc_stress_mode_ ? BRONZE_ABI_PLAIN_OBJECT_BYTES : kLabBytes;
    // allocate_raw may collect (stress does so every time), which zeroes the
    // window — publishing AFTER it returns is what keeps the two ordered.
    void* run = allocate_raw(bytes);
    bronze_tls_block* tls = bronze_tls_block_addr();
    tls->alloc_cursor = reinterpret_cast<uint64_t>(run);
    tls->alloc_limit = tls->alloc_cursor + bytes;
}

static bool is_valid_object_tag(uint16_t tag) noexcept {
    return (tag >= 0xFFF1 && tag <= 0xFFF9) ||
           tag == static_cast<uint16_t>(Tag::BigInt) ||
           tag == static_cast<uint16_t>(Tag::Forwarded);
}

// Does this object's payload hold Values the collector must trace? No for the
// three whose payload is raw bytes — a string's code units, an ArrayBuffer's
// storage, a BigInt's limbs — and reading any of them as Values would forward
// whatever bit pattern happened to look like a heap pointer.
static bool payload_holds_values(uint16_t tag) noexcept {
    return tag != static_cast<uint16_t>(Tag::String) &&
           tag != static_cast<uint16_t>(Tag::RawBytes) &&
           tag != static_cast<uint16_t>(Tag::BigInt);
}

void Heap::forward_value(Value& val) {
    if (!val.isPointer()) {
        return;
    }

    void* payload_ptr = val.asObject<void>();
    if (!payload_ptr) {
        return;
    }

    auto* raw_ptr = static_cast<uint8_t*>(payload_ptr);
    if (raw_ptr < from_space_.base || raw_ptr >= from_space_.bump_ptr) {
        return;
    }

    // Every heap reference in a Value points at the object's HEADER, never
    // its payload — including the ones in out-of-line blocks (object
    // overflow slots, array elements). That invariant is what lets this
    // dereference rather than guess: probing ptr-8 and falling back to ptr,
    // accepting whichever carried a plausible tag, is ambiguous by
    // construction, since an object's last payload word can hold a Value and
    // a Value's low 16 bits can be anything, including a valid-looking tag.
    auto* header = reinterpret_cast<HeapObjectHeader*>(raw_ptr);
    if (!is_valid_object_tag(header->tag)) {
        return;
    }

    if (header->tag == static_cast<uint16_t>(Tag::Forwarded)) {
        auto* new_hdr = *reinterpret_cast<HeapObjectHeader**>(header->payload());
        val = Value::fromTagAndPayload(val.tag(), reinterpret_cast<uintptr_t>(new_hdr));
        return;
    }

    size_t total_size = header->size;
    uint8_t* new_mem = static_cast<uint8_t*>(allocate_in_space(to_space_, total_size));
    std::memcpy(new_mem, header, total_size);
    // THIS is the relocation, so this is where the epoch moves. Anything that
    // hashes an object by its address is wrong from this line onward, and
    // putting the increment at the end of `collect()` instead would make that
    // invalidation depend on a cycle completing rather than on an object
    // actually moving.
    ++relocations_;
    auto* new_hdr = reinterpret_cast<HeapObjectHeader*>(new_mem);

    header->tag = static_cast<uint16_t>(Tag::Forwarded);
    *reinterpret_cast<HeapObjectHeader**>(header->payload()) = new_hdr;

    val = Value::fromTagAndPayload(val.tag(), reinterpret_cast<uintptr_t>(new_hdr));
}

HeapObjectHeader* Heap::survivor_of(HeapObjectHeader* header) const noexcept {
    auto* raw_ptr = reinterpret_cast<uint8_t*>(header);
    // Outside from-space it is not this collection's business at all: an
    // arena-interned key, a to-space address a caller already updated. Reported
    // as a survivor unchanged, because "not in the space being collected" and
    // "died" are different facts and only the second may clear a weak slot.
    if (raw_ptr < from_space_.base || raw_ptr >= from_space_.bump_ptr) {
        return header;
    }
    if (!is_valid_object_tag(header->tag)) {
        return header;
    }
    if (header->tag == static_cast<uint16_t>(Tag::Forwarded)) {
        return *reinterpret_cast<HeapObjectHeader**>(header->payload());
    }
    // Its header still carries the tag it was allocated with, so the copy phase
    // never reached it: nothing live refers to it any more.
    return nullptr;
}

// Diagnostic name for a Tag::Object header's HeapKind word. Pinned to Count
// so adding a kind extends this table instead of printing an index.
static const char* heap_kind_name(uint16_t kind) noexcept {
    static const char* const names[] = {
        "Plain", "Array", "Function", "TypedArray", "ArrayBuffer", "DataView",
        "Map", "Set", "WeakMap", "WeakSet", "Iterator", "RegExp", "Env",
        "ModuleNamespace", "Proxy", "PrivateTable", "WeakRef",
        "FinalizationRegistry",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == HeapKind::Count,
                  "a new HeapKind needs a name here");
    return kind < HeapKind::Count ? names[kind] : "?";
}

void Heap::walk_objects(const std::function<void(HeapObjectHeader*)>& fn) {
    // The same header-run stepping verify_space's pass 1 validates — and the
    // same precondition: the space is a gapless run of live, fully-built
    // objects only immediately after collect() (heap.h has the contract).
    uint8_t* end = from_space_.bump_ptr;
    for (uint8_t* p = from_space_.base; p < end; ) {
        auto* hdr = reinterpret_cast<HeapObjectHeader*>(p);
        fn(hdr);
        p += hdr->size;
    }
}

void Heap::verify_space(const Semispace& space) const {
    const uint8_t* end = space.bump_ptr;
    const uint64_t reservation_lo = reinterpret_cast<uint64_t>(reserved_base_);
    const uint64_t reservation_hi = reservation_lo + reserved_bytes_;

    // Pass 1: the space must parse as a gapless run of headers, because pass 2
    // answers "is this pointer a live object?" by exact membership in this
    // list — a corrupt size here would silently shift every boundary after it.
    std::vector<uint64_t> headers;
    for (const uint8_t* p = space.base; p < end;) {
        auto* hdr = reinterpret_cast<const HeapObjectHeader*>(p);
        const uint16_t t = hdr->tag;
        const bool heap_tag = t == static_cast<uint16_t>(Tag::Object) ||
                              t == static_cast<uint16_t>(Tag::String) ||
                              t == static_cast<uint16_t>(Tag::RawBytes) ||
                              t == static_cast<uint16_t>(Tag::BigInt);
        if (!heap_tag || hdr->size < sizeof(HeapObjectHeader) || (hdr->size & 7) != 0 ||
            p + hdr->size > end) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "heap verify: unwalkable header at +0x%zX: tag=0x%04X flags=%u "
                          "size=%u (live space holds %zu bytes)",
                          static_cast<size_t>(p - space.base), t, hdr->flags, hdr->size,
                          static_cast<size_t>(end - space.base));
            fatal(buf);
        }
        headers.push_back(reinterpret_cast<uint64_t>(p));
        p += hdr->size;
    }

    // Pass 2: every word the collector scans must parse cleanly as a Value.
    // Of the four heap header tags only Tag::Object payloads hold Values
    // (payload_holds_values), so every owner below is an object and its
    // flags word names a HeapKind — which is exactly the name the padding
    // bug class needs reported: the struct type whose scanned word is dirty.
    for (uint64_t addr : headers) {
        auto* hdr = reinterpret_cast<const HeapObjectHeader*>(addr);
        if (!payload_holds_values(hdr->tag)) {
            continue;
        }
        const Value* slots = hdr->payload<const Value>();
        const size_t num_slots = (hdr->size - sizeof(HeapObjectHeader)) / sizeof(Value);
        for (size_t i = 0; i < num_slots; ++i) {
            const Value v = slots[i];
            if (v.isNumber()) {
                continue;
            }
            const char* why = nullptr;
            switch (v.tag()) {
                case static_cast<uint16_t>(Tag::Int32):
                    break;
                case static_cast<uint16_t>(Tag::Bool):
                    if (v.payload() > 1) why = "Bool word whose payload is not 0 or 1";
                    break;
                case static_cast<uint16_t>(Tag::Null):
                case static_cast<uint16_t>(Tag::Undefined):
                case static_cast<uint16_t>(Tag::Hole):
                case static_cast<uint16_t>(Tag::Uninitialized):
                    if (v.payload() != 0) why = "singleton tag carrying a payload";
                    break;
                case static_cast<uint16_t>(Tag::Object):
                case static_cast<uint16_t>(Tag::String):
                case static_cast<uint16_t>(Tag::Symbol):
                case static_cast<uint16_t>(Tag::BigInt): {
                    const uint64_t target = v.payload();
                    // Null is legal (a hardware NaN is 0xFFF8'0000'0000'0000,
                    // which parses as Symbol with payload 0 — forward_value
                    // tolerates it for the same reason). So is anything
                    // outside the reservation: arena-interned symbols,
                    // strings and every C++-owned structure live there.
                    if (target == 0 || target < reservation_lo || target >= reservation_hi) {
                        break;
                    }
                    if (target < reinterpret_cast<uint64_t>(space.base) ||
                        target >= reinterpret_cast<uint64_t>(end)) {
                        why = "pointer into the heap but outside the live space "
                              "(stale semispace reference)";
                    } else if (!std::binary_search(headers.begin(), headers.end(), target)) {
                        why = "pointer into the live space that is not an object header";
                    } else {
                        // A reference's Value tag and its target's header tag
                        // agree, with one designed exception: Tag::Object may
                        // name a RawBytes header (ArrayBuffer stores, WeakRef —
                        // the payloads the scan must skip).
                        const uint16_t vt = v.tag();
                        const uint16_t tt = reinterpret_cast<const HeapObjectHeader*>(target)->tag;
                        const bool ok = vt == static_cast<uint16_t>(Tag::Object)
                                            ? (tt == static_cast<uint16_t>(Tag::Object) ||
                                               tt == static_cast<uint16_t>(Tag::RawBytes))
                                            : tt == vt;
                        if (!ok) why = "pointer whose target header carries a different tag";
                    }
                    break;
                }
                case static_cast<uint16_t>(Tag::Forwarded):
                    why = "Forwarded tag in a scanned word after the copy phase";
                    break;
                default:
                    why = "tag the value model does not define";
                    break;
            }
            if (why) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "heap verify: %s: object at +0x%zX kind=%s size=%u, "
                              "slot %zu = 0x%016llX — a heap struct must zero every "
                              "byte of every word the collector scans",
                              why, static_cast<size_t>(addr - reinterpret_cast<uint64_t>(space.base)),
                              heap_kind_name(hdr->flags), hdr->size, i,
                              static_cast<unsigned long long>(v.rawBits()));
                fatal(buf);
            }
        }
    }
}

void Heap::collect() {
    if (in_gc_) {
        return;
    }

    in_gc_ = true;

    // The inline-allocation window points into from-space, which this
    // collection is about to abandon — invalidate it FIRST, so nothing
    // (hooks included) can see a window over memory whose objects are being
    // forwarded out from under it. bronze_construct refills it later.
    bronze_tls_block_addr()->alloc_cursor = 0;
    bronze_tls_block_addr()->alloc_limit = 0;

    std::chrono::steady_clock::time_point gc_t0;
    if (g_gcLog.enabled) gc_t0 = std::chrono::steady_clock::now();

    if (collection_hook_) {
        collection_hook_(*this);
    }

    to_space_.bump_ptr = to_space_.base;

    for (Value* slot : permanent_roots_) {
        forward_value(*slot);
    }

    RootVisitor visit = [this](Value& slot) { forward_value(slot); };
    for (const RootSource& src : root_sources_) {
        src(visit);
    }

    for (ShadowStackFrame* frame = ShadowStackFrame::current(); frame != nullptr; frame = frame->prev()) {
        Value** root_slots = frame->roots();
        size_t count = frame->count();
        for (size_t i = 0; i < count; ++i) {
            if (root_slots[i]) {
                forward_value(*root_slots[i]);
            }
        }
    }

    // Generated code's root frames: contiguous slot arrays in compiled
    // functions' own stack frames, linked inline by compiled code.
    for (bronze_gc_frame* frame = bronze_tls_block_addr()->frame_top; frame != nullptr;
         frame = frame->prev) {
        Value* slots = reinterpret_cast<Value*>(frame->slots);
        for (uint64_t i = 0; i < frame->count; ++i) {
            forward_value(slots[i]);
        }
    }

    uint8_t* scan_ptr = to_space_.base;
    while (scan_ptr < to_space_.bump_ptr) {
        auto* scan_hdr = reinterpret_cast<HeapObjectHeader*>(scan_ptr);
        size_t obj_size = scan_hdr->size;

        if (payload_holds_values(scan_hdr->tag)) {
            uint8_t* payload_start = reinterpret_cast<uint8_t*>(scan_hdr->payload());
            size_t payload_bytes = obj_size - sizeof(HeapObjectHeader);
            size_t num_slots = payload_bytes / sizeof(Value);
            auto* slots = reinterpret_cast<Value*>(payload_start);
            for (size_t i = 0; i < num_slots; ++i) {
                forward_value(slots[i]);
            }
        }

        scan_ptr += obj_size;
    }

    // Every reachable object has been copied: a from-space header now reads
    // Tag::Forwarded if its object survived and its original tag if it did
    // not, which is exactly the question a finalizer sweep has to ask. It runs
    // BEFORE the swap because that is when "from-space" still names the space
    // the registered pointers point into.
    for (const PostCollectionHook& hook : post_collection_hooks_) {
        hook();
    }

    // After the hooks: they are part of the collection (weak sweeps re-point
    // and clear cells), and the state being certified is the one the mutator
    // resumes on.
    if (gc_verify_mode_) {
        verify_space(to_space_);
    }

    if (g_gcLog.enabled) {
        ++g_gcLog.collections;
        g_gcLog.copied_bytes += to_space_.bump_ptr - to_space_.base;
        g_gcLog.gc_nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - gc_t0)
                                .count();
    }

    // Poison AFTER the hooks: they are the last legitimate reader of the
    // abandoned space (survivor_of decodes its forwarding marks). Anything
    // that reads it after this line was holding a reference across a move.
    if (gc_poison_mode_) {
        std::memset(from_space_.base, 0xDB,
                    static_cast<size_t>(from_space_.bump_ptr - from_space_.base));
    }

    const size_t live_bytes = to_space_.bump_ptr - to_space_.base;
    constexpr size_t kMinThreshold = 16 * 1024 * 1024;
    constexpr size_t kMinHeadroom = 8 * 1024 * 1024;
    const size_t next_threshold = live_bytes + std::max(live_bytes, kMinHeadroom);
    gc_threshold_bytes_ = std::min(semispace_size_, std::max(kMinThreshold, next_threshold));

    from_space_.bump_ptr = from_space_.base;
    std::swap(from_space_, to_space_);

    ++collections_;
    in_gc_ = false;
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
