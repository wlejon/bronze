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
};

}  // namespace bronze::runtime

extern "C" bronze_tls_block* bronze_tls_block_addr(void) {
    return &bronze::runtime::g_tls_block;
}
