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

#include <cstddef>
#include <cstdint>
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
#include <sys/mman.h>
#endif

#include "abi/bronze_abi.h"

static_assert(offsetof(bronze_tls_block, frame_top) == BRONZE_TLS_FRAME_TOP_OFF);
static_assert(offsetof(bronze_tls_block, exception_cell) == BRONZE_TLS_EXCEPTION_CELL_OFF);
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
    /*frame_top=*/nullptr,
    /*exception_cell=*/BRONZE_ABI_NO_EXCEPTION_BITS,
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
};

}  // namespace bronze::runtime

extern "C" bronze_tls_block* bronze_tls_block_addr(void) {
    return &bronze::runtime::g_tls_block;
}

namespace {

struct ShadowStack {
    uint64_t* base = nullptr;
    uint64_t* top = nullptr;

    void init() {
        if (!base) {
            size_t bytes = 64 * 1024 * 1024; // 64 MB virtual address reservation
#if defined(_WIN32)
            base = reinterpret_cast<uint64_t*>(
                VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
            base = reinterpret_cast<uint64_t*>(
                mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#endif
            top = base;
        }
    }
};

static thread_local ShadowStack g_shadow_stack;

}  // namespace

extern "C" bronze_gc_frame* bronze_gc_frame_push(uint32_t count) {
    if (__builtin_expect(!g_shadow_stack.base, 0)) {
        g_shadow_stack.init();
    }
    bronze_tls_block* tls = bronze_tls_block_addr();
    bronze_gc_frame* frame = reinterpret_cast<bronze_gc_frame*>(g_shadow_stack.top);
    g_shadow_stack.top += 2 + count;

    frame->prev = tls->frame_top;
    frame->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        frame->slots[i] = BRONZE_ABI_UNDEFINED_BITS;
    }
    tls->frame_top = frame;
    return frame;
}

extern "C" void bronze_gc_frame_pop(void) {
    bronze_tls_block* tls = bronze_tls_block_addr();
    bronze_gc_frame* frame = tls->frame_top;
    if (__builtin_expect(frame != nullptr, 1)) {
        tls->frame_top = frame->prev;
        g_shadow_stack.top = reinterpret_cast<uint64_t*>(frame);
    }
}
