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
    uint64_t* elem_set_cache_tbl;
    /* The per-KEY inline-cache sites the property helpers consult when a call
     * site brings no entry of its own (rt_prop.cpp). BRONZE_NO_KEY_IC=1
     * lowers it, and every such read then takes the shape walk it always
     * took. */
    uint64_t key_ic_enabled;
    /* The lowest stack address compiled code may run at: every function's
     * prologue compares its stack pointer against it and calls
     * `bronze_stack_overflow` below it, which is how deep recursion becomes
     * `RangeError: Maximum call stack size exceeded` rather than a fault.
     * Armed by `bronze_tls_enter` the first time a thread enters compiled
     * code; zero until then, which no stack pointer is below. */
    uint64_t stack_limit;
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
#define BRONZE_TLS_ELEM_SET_CACHE_TBL_OFF        232
#define BRONZE_TLS_KEY_IC_ENABLED_OFF            240
#define BRONZE_TLS_STACK_LIMIT_OFF               248

/*
 * ---- the pinned register -----------------------------------------------
 *
 * Compiled code keeps the address of this block in a callee-saved register
 * (x64: R13, aarch64: X28) for the whole time it runs, and reads the fields
 * above through it — the exception check after every call, the allocation
 * fast path, the stack-limit check — instead of calling
 * `bronze_tls_block_addr`. Two things keep the register right:
 *
 *   - a module's entry function loads it itself (`bronze_tls_enter`), so
 *     the hosts that call an exported entry directly need to know nothing;
 *   - every other way from C++ into compiled code goes through the runtime's
 *     trampoline (`rtEnterJs`, fn.h), which saves the caller's register,
 *     loads the block, calls, and restores.
 *
 * A compiled function never writes the register, and a C++ helper it calls
 * preserves it by the calling convention, so it survives any depth of
 * JS -> runtime -> JS nesting.
 */

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

#define BRONZE_ABI_ITER_KIND_ARRAY_BITS        0x0000000000000000ull
#define BRONZE_ABI_ITER_KIND_TYPED_ARRAY_BITS  0x4000000000000000ull
#define BRONZE_ABI_ITER_KIND_MAP_ENTRIES_BITS  0x4008000000000000ull
#define BRONZE_ABI_ITER_KIND_MAP_ITERATOR_BITS 0x4018000000000000ull
/* The kinds BELOW this double (5.0: Kind::Protocol) walk a value the runtime
 * owns a cursor into — an array, a string, a typed array, a Map, a Set — and
 * have no iterator object a `return` method could hang off, so closing one is
 * a no-op the inline `iter.close` skips without a call. Every kind at or above
 * it holds an ITERATOR OBJECT in `target` and closes through the helper.
 * Pinned against IterRecordHeader::Kind in runtime/iterator.cpp. */
#define BRONZE_ABI_ITER_KIND_OWNED_LIMIT_BITS 0x4014000000000000ull

#define BRONZE_ABI_MAP_ITER_SLOT_MAP_OFFSET    56
#define BRONZE_ABI_MAP_ITER_SLOT_NEXT_OFFSET   64
#define BRONZE_ABI_MAP_ITER_SLOT_KIND_OFFSET   72

/* A Map's entry table and used-slot count sit in INTERNAL SLOTS after the
 * ordinary object header and its four inline property slots (a Map is a plain
 * object with a real prototype chain): slot 1 and slot 4 of runtime/map.h's
 * CollectionSlot. Pinned against MapHeader in runtime/builtin_map.cpp. */
#define BRONZE_ABI_MAP_HEADER_ENTRIES_OFFSET   64
#define BRONZE_ABI_MAP_HEADER_USED_OFFSET      88

#define BRONZE_ABI_MAP_ITER_KIND_KEYS_BITS     0x0000000000000000ull
#define BRONZE_ABI_MAP_ITER_KIND_VALUES_BITS   0x3FF0000000000000ull
#define BRONZE_ABI_MAP_ITER_KIND_ENTRIES_BITS  0x4000000000000000ull

/* Total bytes of one iteration record — header plus its six Value fields —
 * which is what the inline `iter.open` fast path bump-allocates from the
 * inline-allocation window for an ARRAY source (codegen-llvm/llvm_iter.cpp).
 * Pinned against sizeof(IterRecordHeader) in runtime/iterator.cpp. */
#define BRONZE_ABI_ITER_RECORD_BYTES     56

#ifdef __cplusplus
}
#endif

#endif /* BRONZE_ABI_TLS_H */
