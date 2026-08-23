#ifndef BRONZE_ABI_TLS_H
#define BRONZE_ABI_TLS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bronze_gc_frame bronze_gc_frame;

/*
 * ---- the thread-local state block (TLS) ------------------------------------
 *
 * One block per OS thread executing generated Bronze code. Pinned to a
 * dedicated machine register by the calling convention (X86-64: R14).
 */
typedef struct bronze_tls_block {
    bronze_gc_frame* frame_top;
    uint64_t exception_cell;
    uint64_t proto_epoch;
    uint64_t alloc_cursor;
    uint64_t alloc_limit;
    uint64_t plain_shape;
    uint64_t inline_call_enabled;
    uint64_t array_method_ic_enabled;
    uint64_t inline_overflow_set_enabled;
    uint64_t inline_accessor_enabled;
    uint64_t poly_ic_enabled;
    uint64_t negative_ic_enabled;
    uint64_t elem_ic_enabled;
    uint64_t direct_callout_enabled;
    uint64_t elem_absent_enabled;
    uint64_t fn_singleton_cache_enabled;
    uint64_t* array_method_tbl;
    uint64_t iter_fast_enabled;
    uint64_t inline_roots_enabled;
    uint64_t strict_eq_inline_enabled;
    uint64_t elem_inline_enabled;
    uint64_t* elem_cache_tbl;
    uint64_t method_call_ic_enabled;
    uint64_t elem_key_ic_enabled;
    uint64_t undef_rel_enabled;
    uint64_t sort_fast_enabled;
    uint64_t map_fast_enabled;
    uint64_t ta_set_fast_enabled;
    uint64_t truthy_inline_enabled;
} bronze_tls_block;

#define BRONZE_TLS_FRAME_TOP_OFF                   0
#define BRONZE_TLS_EXCEPTION_CELL_OFF              8
#define BRONZE_TLS_PROTO_EPOCH_OFF                16
#define BRONZE_TLS_ALLOC_CURSOR_OFF               24
#define BRONZE_TLS_ALLOC_LIMIT_OFF                32
#define BRONZE_TLS_PLAIN_SHAPE_OFF                40
#define BRONZE_TLS_INLINE_CALL_ENABLED_OFF        48
#define BRONZE_TLS_ARRAY_METHOD_IC_ENABLED_OFF    56
#define BRONZE_TLS_INLINE_OVERFLOW_SET_ENABLED_OFF 64
#define BRONZE_TLS_INLINE_ACCESSOR_ENABLED_OFF    72
#define BRONZE_TLS_POLY_IC_ENABLED_OFF            80
#define BRONZE_TLS_NEGATIVE_IC_ENABLED_OFF        88
#define BRONZE_TLS_ELEM_IC_ENABLED_OFF            96
#define BRONZE_TLS_DIRECT_CALLOUT_ENABLED_OFF    104
#define BRONZE_TLS_ELEM_ABSENT_ENABLED_OFF       112
#define BRONZE_TLS_FN_SINGLETON_CACHE_ENABLED_OFF 120
#define BRONZE_TLS_ARRAY_METHOD_TBL_OFF          128
#define BRONZE_TLS_ITER_FAST_ENABLED_OFF         136
#define BRONZE_TLS_INLINE_ROOTS_ENABLED_OFF      144
#define BRONZE_TLS_STRICT_EQ_INLINE_ENABLED_OFF  152
#define BRONZE_TLS_ELEM_INLINE_ENABLED_OFF       160
#define BRONZE_TLS_ELEM_CACHE_TBL_OFF            168
#define BRONZE_TLS_METHOD_CALL_IC_ENABLED_OFF    176
#define BRONZE_TLS_ELEM_KEY_IC_ENABLED_OFF       184
#define BRONZE_TLS_UNDEF_REL_ENABLED_OFF         192
#define BRONZE_TLS_SORT_FAST_ENABLED_OFF         200
#define BRONZE_TLS_MAP_FAST_ENABLED_OFF          208
#define BRONZE_TLS_TA_SET_FAST_ENABLED_OFF       216
#define BRONZE_TLS_TRUTHY_INLINE_ENABLED_OFF     224

/*
 * ---- the iteration record, as generated code reads it ---------------------
 */
#define BRONZE_ABI_OBJ_FLAGS_ITERATOR    10
#define BRONZE_ABI_ITER_TARGET_OFFSET     8
#define BRONZE_ABI_ITER_NEXTFN_OFFSET    16
#define BRONZE_ABI_ITER_CURRENT_OFFSET   24
#define BRONZE_ABI_ITER_CURSOR_OFFSET    32
#define BRONZE_ABI_ITER_KIND_OFFSET      40
#define BRONZE_ABI_ITER_DONE_OFFSET      48

#define BRONZE_ABI_ITER_KIND_ARRAY_BITS       0x0000000000000000ull
#define BRONZE_ABI_ITER_KIND_TYPED_ARRAY_BITS 0x4000000000000000ull

/* Total bytes of one iteration record — header plus its six Value fields —
 * which is what the inline `iter.open` fast path bump-allocates from the
 * inline-allocation window for an ARRAY source (codegen-llvm/llvm_iter.cpp).
 * Pinned against sizeof(IterRecordHeader) in runtime/iterator.cpp. */
#define BRONZE_ABI_ITER_RECORD_BYTES     56

#ifdef __cplusplus
}
#endif

#endif /* BRONZE_ABI_TLS_H */
