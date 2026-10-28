// The per-thread ABI data block (bronze_abi.h, `bronze_tls_block`): the one
// definition of the block and the accessor generated code calls once per
// function prologue. The initializers are all compile-time constants, so the
// thread_local is constant-initialized and the accessor is a bare
// TLS-address computation with no init guard on any path.
//
// Per-thread first-touch setup — the BRONZE_NO_* env flags, the exception
// cell's permanent-root registration — happens where each concern already
// lives (Heap's constructor, exception.cpp), not here: the defaults below
// are correct for a thread that has run nothing yet, and every write that
// changes them goes through the runtime, which initializes lazily.

#include "runtime/tls_block.h"
#include "runtime/heap.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
#include "runtime/stack_locals.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#if defined(__APPLE__)
#include <sys/resource.h>
#endif
#endif

#include "abi/bronze_abi.h"

static_assert(offsetof(bronze_tls_block, proto_epoch) == BRONZE_TLS_PROTO_EPOCH_OFF);
static_assert(offsetof(bronze_tls_block, alloc_cursor) == BRONZE_TLS_ALLOC_CURSOR_OFF);
static_assert(offsetof(bronze_tls_block, alloc_limit) == BRONZE_TLS_ALLOC_LIMIT_OFF);
static_assert(offsetof(bronze_tls_block, plain_shape) == BRONZE_TLS_PLAIN_SHAPE_OFF);
static_assert(offsetof(bronze_tls_block, inline_call_enabled) ==
              BRONZE_TLS_INLINE_CALL_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, array_method_ic_enabled) ==
              BRONZE_TLS_ARRAY_METHOD_IC_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, inline_overflow_set_enabled) ==
              BRONZE_TLS_INLINE_OVERFLOW_SET_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, inline_accessor_enabled) ==
              BRONZE_TLS_INLINE_ACCESSOR_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, poly_ic_enabled) == BRONZE_TLS_POLY_IC_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, negative_ic_enabled) ==
              BRONZE_TLS_NEGATIVE_IC_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, elem_ic_enabled) == BRONZE_TLS_ELEM_IC_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, direct_callout_enabled) ==
              BRONZE_TLS_DIRECT_CALLOUT_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, elem_absent_enabled) ==
              BRONZE_TLS_ELEM_ABSENT_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, fn_singleton_cache_enabled) ==
              BRONZE_TLS_FN_SINGLETON_CACHE_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, array_method_tbl) == BRONZE_TLS_ARRAY_METHOD_TBL_OFF);
static_assert(offsetof(bronze_tls_block, iter_fast_enabled) == BRONZE_TLS_ITER_FAST_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, inline_roots_enabled) ==
              BRONZE_TLS_INLINE_ROOTS_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, strict_eq_inline_enabled) ==
              BRONZE_TLS_STRICT_EQ_INLINE_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, elem_inline_enabled) ==
              BRONZE_TLS_ELEM_INLINE_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, elem_cache_tbl) == BRONZE_TLS_ELEM_CACHE_TBL_OFF);
static_assert(offsetof(bronze_tls_block, method_call_ic_enabled) ==
              BRONZE_TLS_METHOD_CALL_IC_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, elem_key_ic_enabled) ==
              BRONZE_TLS_ELEM_KEY_IC_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, undef_rel_enabled) ==
              BRONZE_TLS_UNDEF_REL_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, sort_fast_enabled) ==
              BRONZE_TLS_SORT_FAST_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, map_fast_enabled) == BRONZE_TLS_MAP_FAST_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, ta_set_fast_enabled) ==
              BRONZE_TLS_TA_SET_FAST_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, truthy_inline_enabled) ==
              BRONZE_TLS_TRUTHY_INLINE_ENABLED_OFF);
static_assert(offsetof(bronze_tls_block, elem_set_cache_tbl) ==
              BRONZE_TLS_ELEM_SET_CACHE_TBL_OFF);
static_assert(offsetof(bronze_tls_block, stack_limit) == BRONZE_TLS_STACK_LIMIT_OFF);
static_assert(offsetof(bronze_tls_block, module_deltas) == BRONZE_TLS_MODULE_DELTAS_OFF);
static_assert(offsetof(bronze_tls_block, gc_cell_header) == BRONZE_TLS_GC_CELL_HEADER_OFF);
static_assert(offsetof(bronze_tls_block, realm_slot) == BRONZE_TLS_REALM_SLOT_OFF);
// The inline-allocation window is the collector's young bump region in place
// (heap.cpp, bind_thread): brass's {top, end} pair.
static_assert(offsetof(bronze_tls_block, alloc_limit) ==
              offsetof(bronze_tls_block, alloc_cursor) + offsetof(brass::gc::Heap::AllocationBuffer, end));
static_assert(offsetof(brass::gc::Heap::AllocationBuffer, top) == 0);
static_assert(sizeof(brass::gc::Heap::AllocationBuffer) == 2 * sizeof(uint64_t));

namespace bronze::runtime {

// proto_epoch starts at 1 so that a zeroed IC entry (epoch word 0) can never
// match a live epoch; the enable flags start at 1 and are lowered per thread
// by Heap's constructor under their BRONZE_NO_* env vars.
//
// At namespace scope rather than inside the accessor, so that tls_block.h can
// name it and the runtime's own seam predicates become three instructions
// instead of a call. The initializer is still all compile-time constants, so
// this is still constant-initialized with no guard.
thread_local bronze_tls_block g_tls_block = {
    /*proto_epoch=*/1,
    /*alloc_cursor=*/0,
    /*alloc_limit=*/0,
    /*plain_shape=*/0,
    /*inline_call_enabled=*/1,
    /*array_method_ic_enabled=*/1,
    /*inline_overflow_set_enabled=*/1,
    /*inline_accessor_enabled=*/1,
    /*poly_ic_enabled=*/1,
    /*negative_ic_enabled=*/1,
    /*elem_ic_enabled=*/1,
    /*direct_callout_enabled=*/1,
    /*elem_absent_enabled=*/1,
    /*fn_singleton_cache_enabled=*/1,
    /*array_method_tbl=*/nullptr,
    /*iter_fast_enabled=*/1,
    /*inline_roots_enabled=*/1,
    /*strict_eq_inline_enabled=*/1,
    /*elem_inline_enabled=*/1,
    /*elem_cache_tbl=*/nullptr,
    /*method_call_ic_enabled=*/1,
    /*elem_key_ic_enabled=*/1,
    /*undef_rel_enabled=*/1,
    /*sort_fast_enabled=*/1,
    /*map_fast_enabled=*/1,
    /*ta_set_fast_enabled=*/1,
    /*truthy_inline_enabled=*/1,
    /*elem_set_cache_tbl=*/nullptr,
    /*stack_limit=*/0,
    /*module_deltas=*/nullptr,
    /*gc_cell_header=*/0,
    /*realm_slot=*/0,
};

namespace {

// The thread's stack, [low, high), from the OS; both zero when it cannot be
// asked, which leaves the limit disarmed rather than wrong.
void threadStackBounds(uintptr_t& low, uintptr_t& high) {
    low = 0;
    high = 0;
#if defined(_WIN32)
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    low = static_cast<uintptr_t>(lo);
    high = static_cast<uintptr_t>(hi);
#elif defined(__APPLE__)
    char marker = 0;
    const uintptr_t cur_sp = reinterpret_cast<uintptr_t>(&marker);
    uintptr_t addr = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    size_t size = pthread_get_stacksize_np(pthread_self());
    if (pthread_main_np()) {
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0) {
            if (rl.rlim_cur >= RLIM_INFINITY || rl.rlim_cur == 0) {
                size = 8 * 1024 * 1024;
            } else {
                size = static_cast<size_t>(rl.rlim_cur);
            }
        } else {
            size = 8 * 1024 * 1024;
        }
    }
    if (addr > cur_sp) {
        high = addr;
        low = (high > size) ? (high - size) : 0;
    } else {
        low = addr;
        high = low + size;
    }
#else
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
            low = reinterpret_cast<uintptr_t>(addr);
            high = low + size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
}

// What the check leaves below the limit: room for the runtime to build the
// RangeError — its message, its stack trace, the property store — and for
// whatever the host does with it, all on the same stack. A quarter of a
// small stack, so a 256 KB worker still gets three quarters of it.
constexpr uintptr_t kStackReserveBytes = 128 * 1024;

}  // namespace

}  // namespace bronze::runtime

extern "C" bronze_tls_block* bronze_tls_block_addr(void) {
    return &bronze::runtime::g_tls_block;
}

namespace bronze::runtime {

namespace {

// The running stack's real limit (tls_block.h). Per stack: a fiber starts
// with none, and whoever runs code on it sets one from its bounds
// (compartment.cpp); the block's copy follows it.
thread_local uintptr_t g_stackLimit = 0;

std::atomic_ref<uint64_t> blockLimit(bronze_tls_block* tls) noexcept {
    return std::atomic_ref<uint64_t>(tls->stack_limit);
}

// Registered by hand rather than with registerStackLocalWord: the restore
// also puts the entered stack's limit in the block, which an interrupt armed
// for the stack being left must not follow onto it.
[[maybe_unused]] const int g_stackLimitLocal = [] {
    brass::gc::StackLocal local;
    local.save = [](void** slot) { *slot = reinterpret_cast<void*>(g_stackLimit); };
    local.restore = [](void** slot, bool fresh) {
        g_stackLimit = fresh ? 0 : reinterpret_cast<uintptr_t>(*slot);
        blockLimit(&g_tls_block).store(g_stackLimit, std::memory_order_relaxed);
    };
    return brass::gc::register_stack_local(local);
}();

}  // namespace

uintptr_t rtStackLimit() noexcept { return g_stackLimit; }

void rtSetStackLimit(uintptr_t limit) noexcept {
    g_stackLimit = limit;
    blockLimit(&g_tls_block).store(limit, std::memory_order_relaxed);
}

uintptr_t rtStackLimitFor(uintptr_t low, uintptr_t high) noexcept {
    if (high <= low) return 0;
    const uintptr_t size = high - low;
    const uintptr_t reserve = size >= 4 * kStackReserveBytes ? kStackReserveBytes : size / 4;
    return low + reserve;
}

void rtArmInterrupt(bronze_tls_block* tls) noexcept {
    blockLimit(tls).store(kInterruptArmed, std::memory_order_relaxed);
}

void rtDisarmInterrupt() noexcept {
    blockLimit(&g_tls_block).store(g_stackLimit, std::memory_order_relaxed);
}

}  // namespace bronze::runtime

extern "C" void* bronze_tls_enter(void) {
    bronze_tls_block* tls = &bronze::runtime::g_tls_block;
    uintptr_t limit = bronze::runtime::rtStackLimit();
    if (BRONZE_UNLIKELY(limit == 0)) {
        // The thread's first entry into generated code. That code calls brass
        // directly (brass_coro_create allocates a generator's frame on
        // Heap::current()), so the thread's heap is bound before it runs, not
        // on the runtime's first allocation.
        bronze::runtime::rtHeap();
        uintptr_t low = 0, high = 0;
        bronze::runtime::threadStackBounds(low, high);
        limit = bronze::runtime::rtStackLimitFor(low, high);
    }
    char marker = 0;
    const uintptr_t cur_sp = reinterpret_cast<uintptr_t>(&marker);
    if (limit != 0 && cur_sp <= limit + 32 * 1024) {
        // Near or past the limit. When the limit is the one this thread's
        // own stack gives and the entry is on that stack, a recursion that
        // passes through the runtime (a call the runtime makes for compiled
        // code, every call of an interpreted one) has reached it, and the
        // callee's prologue raising the RangeError is the answer: moving the
        // limit down at each such entry let the recursion run off the stack.
        // Any other limit near this entry does not describe the stack it is
        // on (bounds misread, or a stack the limit was not computed for), and
        // the entry gets the reserve below where it stands.
        uintptr_t low = 0, high = 0;
        bronze::runtime::threadStackBounds(low, high);
        const bool own = low < cur_sp && cur_sp <= high && limit == bronze::runtime::rtStackLimitFor(low, high);
        if (!own) {
            limit = cur_sp > bronze::runtime::kStackReserveBytes ? cur_sp - bronze::runtime::kStackReserveBytes : 0;
        }
    }
    // Through the setter: an interrupt armed on this thread stays armed only
    // if nothing changed (the store below would drop it otherwise, and the
    // scheduler's next tick arms it again).
    if (limit != bronze::runtime::rtStackLimit()) bronze::runtime::rtSetStackLimit(limit);
    return tls;
}

// A loop backedge's interrupt poll (brass Module::interrupt_poll) found the
// stack pointer below `stack_limit`, which in a loop means the limit is the
// armed one: put the real limit back and service the interrupt.
extern "C" void bronze_interrupt_poll(void) {
    bronze::runtime::rtDisarmInterrupt();
    bronze::runtime::rtServiceInterrupt();
}

