#ifndef BRONZE_ABI_TLS_H
#define BRONZE_ABI_TLS_H

#include <stdint.h>
#include <stdbool.h>

#ifndef BRONZE_ABI_OBJ_OVERFLOW_SLOT
#include "abi/bronze_abi_layout.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ---- the thread-local state block (TLS) ------------------------------------
 *
 * One block per OS thread executing generated Bronze code. Pinned to a
 * dedicated machine register by the calling convention (x64: R13, aarch64: X28).
 */
/* The megamorphic property cache's geometry (mega_prop below). An entry's
 * index is ((shape ^ key * MUL) * MUL) >> (64 - BITS), computed the same way
 * by the runtime and by generated code; a write entry's key carries
 * SET_KEY_BIT, since a read entry says nothing about writability. */
#define BRONZE_ABI_MEGA_PROP_BITS        10
#define BRONZE_ABI_MEGA_PROP_ENTRIES     (1u << BRONZE_ABI_MEGA_PROP_BITS)
#define BRONZE_ABI_MEGA_PROP_ENTRY_WORDS 4
#define BRONZE_ABI_MEGA_PROP_KEY_WORD    3
#define BRONZE_ABI_MEGA_PROP_HASH_MUL    0x9E3779B97F4A7C15ULL
#define BRONZE_ABI_MEGA_PROP_SET_KEY_BIT 0x80000000u

typedef struct bronze_tls_block {
    uint64_t proto_epoch;
    uint64_t alloc_cursor;
    uint64_t alloc_limit;
    uint64_t plain_shape;
    uint64_t* array_method_tbl;
    uint64_t* elem_cache_tbl;
    uint64_t* elem_set_cache_tbl;
    /* The lowest stack address compiled code may run at: every function's
     * prologue compares its stack pointer against it and calls
     * `bronze_stack_overflow` below it, which is how deep recursion becomes
     * `RangeError: Maximum call stack size exceeded` rather than a fault.
     * Armed by `bronze_tls_enter` the first time a thread enters compiled
     * code; zero until then, which no stack pointer is below. */
    uint64_t stack_limit;
    /* This thread's module DELTAS, indexed by a module's slot cell: the
     * offset from the image's writable tables to this thread's own copy of
     * them (bronze_abi.h, `bronze_module_instance`). Generated code adds the
     * entry at its module's slot to every table address, so one image run on
     * several threads keeps one set of inline caches, global cache, template
     * cells, environment cell and import table per thread. Null until the
     * thread runs its first module entry; grown by the runtime, never freed. */
    uint64_t* module_deltas;
    /* The collector's header word for an inline allocation: brass's object
     * header (payload bytes in the low 32 bits, layout id in the next 16)
     * with the size left zero. The inline fast path ORs in its object's size
     * and stores it one word before the bronze header it writes. The layout
     * id is registered at run time, which is why it is read from here and
     * not compiled in. */
    uint64_t gc_cell_header;
    /* The slot of the realm compiled code on this thread is running in
     * (realm.h, rtCurrentRealmSlot): 0 before any realm exists. A call from
     * compiled code enters a function's `code` without the runtime only when
     * the function was made in this realm (bronze_abi.h,
     * BRONZE_ABI_FN_FLAG_DIRECT_CALL); any other callee goes through the
     * runtime, which enters its realm first. Kept equal to the realm stack's
     * top by the runtime at every change of it, a stack switch included. */
    uint64_t realm_slot;
    /* The typed-array `length` table (layout: BRONZE_ABI_TA_LENGTH_TBL_*):
     * the prototype-mutation epoch at which this thread's typed-array family
     * was last verified pristine (0 when it was not, which no live epoch
     * is), then each kind's intrinsic instance shape. What lets generated
     * code answer `view.length` from the header exactly when the runtime's
     * read would (rtTypedArrayChainPristine): the view's shape is its kind's
     * instance shape and the latch is for the current epoch. Points at a
     * static all-zero table until the intrinsics are built, so it is never
     * null and never hits before then. */
    uint64_t* ta_length_tbl;
    /* The prototype-mutation epoch at which this thread's ordinary array
     * chain — `Array.prototype` and what lies above it — was last found to
     * hold no array-index key (0 when it was not, which no live epoch is).
     * A hole reads through the chain (10.4.2 leaves it to OrdinaryGet), so
     * this is what lets an array with no box treat a hole as `undefined`
     * and as absent without asking. The runtime's latch, consulted only once
     * array_index_hazard (below) is set (rt_array_holes.cpp,
     * rtArrayChainHoldsIndex); generated code reads the hazard instead,
     * which is never stale. */
    uint64_t array_hole_epoch;
    /* This thread's interned key headers, indexed by global key id
     * (rt_key.cpp's per-thread table, mirrored here after every change of
     * its storage): entry i is the arena StringHeader* for key i, or 0 when
     * this thread has not interned it yet; `key_header_count` entries.
     * What lets generated code box a string literal (`box.str k`, which is
     * bronze_box_str_key) with two loads instead of a call: an entry that is
     * there, tagged as a String, is exactly the helper's answer, and an index
     * at or past the count or a 0 entry calls the helper, which interns it.
     * Null / 0 on a thread that has interned nothing. */
    uint64_t* key_header_tbl;
    uint64_t key_header_count;
    /* 0xFFFFFFFF once any object that is (or became) a prototype could answer an
     * array index: an index-like key added to one, one holding such a key
     * marked as a prototype, or a link that is no ordinary object put on a
     * chain (runtime/object.h, tripArrayIndexHazard). Sticky. While it is 0
     * no chain holds an index, so a hole reads as `undefined` and a store
     * into a hole or at `length` lands on the array without asking the
     * chain; generated code's element paths test it only on a hole or an
     * append (and once per dense-array guard), and send those to the
     * runtime when it is set, which then asks the chain precisely
     * (rt_array_holes.cpp). The set value is all ones in the low 32 bits so
     * an inline push or shift folds it into a bound it tests anyway (an
     * index OR'd with it is below no capacity; it is below no length). */
    uint64_t array_index_hazard;
    /* The same fact in the form a dense-array guard spends it: the tag mask
     * the guard ANDs an array's `elements` word with before asking for the
     * object tag — 0xFFFF000000000000 while the hazard is clear, 0 once it
     * is set, which no tag compare passes. The guard was going to AND with a
     * mask anyway, so taking it from here (one memory operand) is the whole
     * of what the hazard costs it; a separate load and test of the word
     * above was +2.8% instructions on three_math. Set with it, never apart. */
    uint64_t array_elems_tag_mask;
    /* The young generation of the heap this thread allocates in, as
     * [young_base, young_base + young_span): brass's young_base() and
     * young_span(), fixed for the heap's lifetime and published when the
     * thread binds to it (span 0 while it is bound to none, which no address
     * is inside). A store into an object in that range needs no write
     * barrier (brass's slow path is only for a target in the old range,
     * which the young range never overlaps), so compiled code tests the
     * target against it before calling brass_gc_write_barrier — the store a
     * fresh object takes, an iteration record's `current`, a literal's
     * fields — and calls only for an object outside it. The test is brass's
     * lowering of MIR's write_barrier, which reads the range here (brass
     * Module::set_young_range). */
    uint64_t young_base;
    uint64_t young_span;
    /* The megamorphic property cache (runtime/rt_prop_mega.h): a direct-
     * mapped table of BRONZE_ABI_MEGA_PROP_ENTRIES entries keyed by (shape,
     * key), each an IC way (shape, slot word, epoch: the InlineCache layout,
     * BRONZE_ABI_IC_*_OFFSET) followed by the key word. A read site whose
     * ways all miss probes it before the helper's full lookup, and the helper
     * fills it from what the site installed. In the block itself rather than
     * behind a pointer, so a probe is pinned-register relative. */
    uint64_t mega_prop[BRONZE_ABI_MEGA_PROP_ENTRIES * BRONZE_ABI_MEGA_PROP_ENTRY_WORDS];
    /* An object literal's root shape by its inline capacity: entry N is the
     * root over %Object.prototype% whose objects have N inline slots
     * (BRONZE_ABI_OBJ_INLINE_SLOTS..BRONZE_ABI_OBJ_OVERFLOW_SLOT; the entry
     * for BRONZE_ABI_OBJ_INLINE_SLOTS is `plain_shape`), or 0 until the
     * runtime has made it. What a sized literal's inline allocation reads;
     * a 0 entry takes the helper, which makes it. */
    uint64_t plain_shapes[BRONZE_ABI_OBJ_OVERFLOW_SLOT + 1];
} bronze_tls_block;

#define BRONZE_TLS_PROTO_EPOCH_OFF                 0
#define BRONZE_TLS_ALLOC_CURSOR_OFF                8
#define BRONZE_TLS_ALLOC_LIMIT_OFF                16
#define BRONZE_TLS_PLAIN_SHAPE_OFF                24
#define BRONZE_TLS_ARRAY_METHOD_TBL_OFF           32
#define BRONZE_TLS_ELEM_CACHE_TBL_OFF             40
#define BRONZE_TLS_ELEM_SET_CACHE_TBL_OFF         48
#define BRONZE_TLS_STACK_LIMIT_OFF                56
#define BRONZE_TLS_MODULE_DELTAS_OFF              64
#define BRONZE_TLS_GC_CELL_HEADER_OFF             72
#define BRONZE_TLS_REALM_SLOT_OFF                 80
#define BRONZE_TLS_TA_LENGTH_TBL_OFF              88
#define BRONZE_TLS_ARRAY_HOLE_EPOCH_OFF           96
#define BRONZE_TLS_KEY_HEADER_TBL_OFF            104
#define BRONZE_TLS_KEY_HEADER_COUNT_OFF          112
#define BRONZE_TLS_ARRAY_INDEX_HAZARD_OFF        120
#define BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF      128
#define BRONZE_TLS_YOUNG_BASE_OFF                136
#define BRONZE_TLS_YOUNG_SPAN_OFF                144
#define BRONZE_TLS_MEGA_PROP_OFF                 152
#define BRONZE_TLS_PLAIN_SHAPES_OFF              (BRONZE_TLS_MEGA_PROP_OFF + \
    8 * BRONZE_ABI_MEGA_PROP_ENTRIES * BRONZE_ABI_MEGA_PROP_ENTRY_WORDS)

/*
 * ---- the constants below the block ---------------------------------------
 *
 * Sixteen 64-bit words just below the block, at [pinned register - 128] to
 * [pinned register - 8], hold the constants guards compare with most: the
 * NaN-box tags, the payload mask and limit, the number range's end. x64
 * code reads one there (brass Module::add_pinned_constant) instead of from its
 * function's constant pool, `[r13 - d8]` against `[rip + d32]`, three bytes
 * shorter, and loads one into a register the same way instead of a 10-byte
 * movabs: in three.js's per-draw path those were a sixth of the bytes the
 * front end fetched. Never written; the block's thread_local holder
 * (runtime/tls_block.cpp, bronze_tls_area) initializes them from this list,
 * and the module registers the same list, so the two cannot disagree.
 * Constant i is at BRONZE_TLS_CONSTANT_OFF(i).
 */
#define BRONZE_TLS_CONSTANT_COUNT 16
#define BRONZE_TLS_CONSTANT_OFF(i) (-8 * (BRONZE_TLS_CONSTANT_COUNT - (i)))
#define BRONZE_TLS_CONSTANTS(X)                                                 \
    X(0xFFF1000000000000ull) /* an object's tag (BRONZE_ABI_TAG_OBJECT) */      \
    X(0x0001000000000000ull) /* 2^48: an untagged payload is below it */        \
    X(BRONZE_ABI_NUMBER_MAX_BITS)                                               \
    X(BRONZE_ABI_VALUE_PAYLOAD_MASK)                                            \
    X(0x0000000100000000ull) /* 2^32 */                                         \
    X(BRONZE_ABI_UNDEFINED_BITS)                                                \
    X(BRONZE_ABI_CANONICAL_NAN_BITS)                                            \
    X(0xFFFF000000000000ull) /* the tag bits */                                 \
    X(BRONZE_ABI_HOLE_BITS)                                                     \
    X(0xFFF4000000000000ull) /* false (BRONZE_ABI_TAG_BOOL) */                  \
    X(BRONZE_ABI_TRUE_BITS)                                                     \
    X(0x4000000000000000ull)                                                    \
    X(0xFFFFFF0000000090ull)                                                    \
    X(0xFFFA000000000000ull)                                                    \
    X(0x3FF0000000000000ull) /* 1.0 */                                          \
    X(0xFFF5000000000000ull) /* null (BRONZE_ABI_TAG_NULL) */

/* The block's holder: the constants, then the block, which the pinned
 * register and bronze_tls_block_addr point at. */
typedef struct bronze_tls_area {
    uint64_t constants[BRONZE_TLS_CONSTANT_COUNT];
    bronze_tls_block block;
} bronze_tls_area;

/*
 * ---- the pinned register -----------------------------------------------
 *
 * Compiled code keeps the address of this block in a callee-saved register
 * (x64: R13, aarch64: X28) for the whole time it runs, and reads the fields
 * above through it — the inline caches, the allocation fast path, the
 * stack-limit check — instead of calling
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
