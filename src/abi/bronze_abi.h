#ifndef BRONZE_ABI_H
#define BRONZE_ABI_H

/*
 * The ABI between the C++ runtime and LLVM-generated code: every symbol
 * generated code links against, and nothing else. This header is pure C
 * (abi_check.c compiles it as C to enforce that): C cannot express a C++
 * class, so a type whose calling convention differs from its bit pattern
 * (MSVC returns classes with user-defined constructors via a hidden sret
 * pointer, shifting every argument register) is unrepresentable here.
 *
 * The registry below is the single source of truth. It expands twice:
 * into the C prototypes at the bottom of this header (which the runtime's
 * definitions are checked against), and into the LLVM declarations in
 * codegen-llvm (see BRONZE_ABI_LLVM_TYPES there). Adding a helper is one
 * X(...) line; a signature drift between the two sides is therefore
 * structurally impossible.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one function-pointer shape generated code is entered through
 * (function objects' code pointers). Primitives only, by construction.
 * `env_bits` carries the closure's environment record;
 * `undefined` for a function that captures nothing. */
typedef uint64_t (*bronze_fn_code)(uint64_t env_bits, uint64_t this_bits, uint32_t argc,
                                   const uint64_t* argv_bits);

/* Bit patterns of the NaN-boxed `undefined` and `null` values, so generated
 * code can materialize them as plain i64 constants. Pinned against the
 * runtime's Value constructors by static_asserts in rt_helpers.cpp. */
#define BRONZE_ABI_UNDEFINED_BITS 0xFFF6000000000000ull
#define BRONZE_ABI_NULL_BITS      0xFFF5000000000000ull

/* The Hole singleton, which is what `bronze_exception_cell` holds when no
 * exception is pending. The Hole is internal by construction — the value model
 * forbids it from ever being a user-visible value — so it can mean "empty"
 * without colliding with anything throwable, and its payload is 0, so "is
 * something pending?" is one
 * 64-bit compare against this constant rather than a mask and a shift. A
 * separate boolean flag was rejected for the reason two words always are:
 * they can disagree. */
#define BRONZE_ABI_NO_EXCEPTION_BITS 0xFFF7000000000000ull

/* A function object whose `name` was never recorded, as the key index
 * `bronze_create_function` and `bronze_function_singleton` take.
 *
 * Every function the COMPILER creates has one — 10.2.9 SetFunctionName gives
 * even an anonymous function expression the empty string — so this is not
 * "anonymous". It is the runtime's own native builtins, which are function
 * objects made from a C function pointer and have no key index to name: for
 * those `f.name` and `f.length` stay the named hard error they have always
 * been, rather than answering "" and 0, which would be two wrong facts about
 * `Object.keys`. */
#define BRONZE_ABI_FN_NAME_NONE 0xFFFFFFFFu

/* The uninitialized-binding singleton (runtime/value.h Tag::Uninitialized):
 * what an environment slot holds for a `let`, `const` or `class` binding
 * between the moment its scope is entered and the moment its declaration is
 * evaluated. Generated code materializes it directly — `env.init.tdz` is a
 * plain `bronze_env_set` of this constant — so no helper exists to produce
 * it, and no helper ever returns it. Pinned against the runtime's Value
 * constructor by a static_assert in rt_object.cpp. */
#define BRONZE_ABI_UNINITIALIZED_BITS 0xFFFA000000000000ull

/*
 * X(name, RET, PARAMS)
 *   RET    — one BRONZE_ABI_* type token (BRONZE_ABI_VOID for none)
 *   PARAMS — parenthesized comma list of type tokens;
 *            (BRONZE_ABI_NOARGS) for an empty parameter list
 */
#define BRONZE_ABI_FUNCTIONS(X) \
    X(bronze_truthy,              BRONZE_ABI_BOOL, (BRONZE_ABI_U64)) \
    X(bronze_is_nullish,          BRONZE_ABI_BOOL, (BRONZE_ABI_U64)) \
    X(bronze_strict_eq,           BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_loose_eq,            BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_rel_lt,              BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_rel_gt,              BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_rel_le,              BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_rel_ge,              BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_typeof,              BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_to_string,           BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_instanceof,          BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_has_property,        BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_to_int32,            BRONZE_ABI_I32,  (BRONZE_ABI_U64)) \
    X(bronze_to_int32_f64,        BRONZE_ABI_I32,  (BRONZE_ABI_F64)) \
    X(bronze_pow,                 BRONZE_ABI_F64,  (BRONZE_ABI_F64, BRONZE_ABI_F64)) \
    X(bronze_box_f64,             BRONZE_ABI_U64,  (BRONZE_ABI_F64)) \
    X(bronze_box_i32,             BRONZE_ABI_U64,  (BRONZE_ABI_I32)) \
    X(bronze_box_bool,            BRONZE_ABI_U64,  (BRONZE_ABI_BOOL)) \
    X(bronze_box_str,             BRONZE_ABI_U64,  (BRONZE_ABI_CSTR)) \
    X(bronze_box_str_key,         BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_unbox_f64,           BRONZE_ABI_F64,  (BRONZE_ABI_U64)) \
    X(bronze_unbox_i32,           BRONZE_ABI_I32,  (BRONZE_ABI_U64)) \
    X(bronze_unbox_bool,          BRONZE_ABI_BOOL, (BRONZE_ABI_U64)) \
    X(bronze_create_object,       BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    X(bronze_create_generator_object, BRONZE_ABI_U64, (BRONZE_ABI_U64)) \
    X(bronze_async_machine,       BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_start,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_await,         BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_module_namespace,    BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_object_keys,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_for_in_keys,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_method_def,          BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_method_def_computed, BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_accessor_def,        BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_prop_delete,         BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_BOOL)) \
    X(bronze_elem_delete,         BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_global_get,          BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_reference_error,     BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_arguments_object,    BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_arg_at,              BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_U32)) \
    X(bronze_class_extends,       BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_create_array,        BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_create_function,     BRONZE_ABI_U64,  (BRONZE_ABI_FNPTR, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_env_create,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_env_get,             BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_env_get_tdz,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_env_set,             BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_module_env_set,      BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_module_env_get,      BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    X(bronze_prop_get,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_MU64)) \
    X(bronze_super_get,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_super_set,           BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_prop_set,            BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_MU64, BRONZE_ABI_BOOL)) \
    X(bronze_elem_get,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_iter_open,           BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_iter_step,           BRONZE_ABI_BOOL, (BRONZE_ABI_U64)) \
    X(bronze_iter_value,          BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_iter_close,          BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_iter_rest,           BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_iter_delegate,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_pattern_check,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_rest_args,           BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_U32)) \
    X(bronze_array_append,        BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_array_spread,        BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_object_spread,       BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_object_rest,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_call_spread, BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_spread,    BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_elem_set,            BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_dynamic_call,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_construct,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_function_singleton,  BRONZE_ABI_U64,  (BRONZE_ABI_FNPTR, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_string_concat,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_add,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_print_value,         BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_values,        BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_print_string,        BRONZE_ABI_VOID, (BRONZE_ABI_CSTR)) \
    X(bronze_print_value_err,     BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_values_err,    BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_print_spread,        BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_spread_err,    BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_register_key_string, BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_CSTR)) \
    X(bronze_uncaught_exception,  BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS))

/*
 * Data symbols generated code links against. Same single-source-of-truth
 * rule as the function registry: never hand-declare one in the backend.
 *
 * X(name, TYPE)
 */
#define BRONZE_ABI_GLOBALS(X) \
    X(bronze_gc_frame_top,   BRONZE_ABI_FRAMEPTR) \
    X(bronze_exception_cell, BRONZE_ABI_U64)

/*
 * ---- the inline property cache contract ---------------------------------
 *
 * The IC table is a zero-initialized global array in the GENERATED object
 * file, one BRONZE_ABI_IC_ENTRY_SIZE-byte entry per property site, and the
 * entry pointer is what `bronze_prop_get` / `bronze_prop_set` take (the
 * BRONZE_ABI_MU64 above) instead of an index into a runtime vector. A vector
 * reallocates, so generated code could not hold a pointer into it, so the
 * check had to happen inside the helper and the CALL was most of the cost.
 *
 * Generated code loads these fields itself, so the runtime's C++ layouts
 * below are part of this ABI, not private to the runtime. object.h and
 * rt_helpers.cpp static_assert every constant here against the real struct,
 * so adding a field to `InlineCache`, `HeapObjectHeader` or `ObjectHeader`
 * is a compile error rather than a silent miscompile.
 *
 * The entry is four plain words the collector never touches: shapes are
 * immortal and non-moving, and the holder is derived from `cached_depth`
 * rather than cached.
 *
 * The fourth word is the prototype-mutation epoch the entry was filled at,
 * and it is what makes a depth > 0 entry sound: the receiver's shape cannot
 * notice a property added to an object BETWEEN the receiver and the holder,
 * because that add changes only the intermediate's shape. The inline fast
 * path never reads it — that path is depth 0 only, where the receiver's own
 * shape is the whole answer — so this word costs generated code the table
 * stride and nothing else.
 */
#define BRONZE_ABI_IC_ENTRY_SIZE     24 /* sizeof(InlineCache) */
#define BRONZE_ABI_IC_SHAPE_OFFSET    0 /* InlineCache::cached_shape (pointer) */
#define BRONZE_ABI_IC_SLOT_OFFSET     8 /* InlineCache::cached_slot  (uint32) */
#define BRONZE_ABI_IC_DEPTH_OFFSET   12 /* InlineCache::cached_depth (uint32) */
#define BRONZE_ABI_IC_EPOCH_OFFSET   16 /* InlineCache::cached_epoch (uint64) */
/* slot and depth are adjacent and little-endian, so the single u64 at
 * IC_SLOT_OFFSET is (depth << 32) | slot. `that word < kInlineSlots` is
 * therefore ONE compare meaning "own property, in an inline slot" — the
 * exact envelope the inline fast path covers. Reading only the low half
 * would forget the depth and return an ancestor's slot off the receiver,
 * which is the bug the cached depth exists to prevent. */
#define BRONZE_ABI_IC_SLOTWORD_OFFSET BRONZE_ABI_IC_SLOT_OFFSET

/* Value: NaN-boxed, tag in the top 16 bits (runtime/value.h). */
#define BRONZE_ABI_VALUE_TAG_SHIFT      48
#define BRONZE_ABI_VALUE_PAYLOAD_MASK   0x0000FFFFFFFFFFFFull
#define BRONZE_ABI_TAG_OBJECT           0xFFF1
#define BRONZE_ABI_TAG_STRING           0xFFF2
#define BRONZE_ABI_TAG_INT32            0xFFF3
#define BRONZE_ABI_TAG_BOOL             0xFFF4
#define BRONZE_ABI_TAG_NULL             0xFFF5
#define BRONZE_ABI_TAG_UNDEFINED        0xFFF6
#define BRONZE_ABI_CANONICAL_NAN_BITS   0x7FF8000000000000ull
#define BRONZE_ABI_NUMBER_MAX_BITS      0xFFF0000000000000ull

/* HeapObjectHeader::flags, and the value that means "a plain object" as
 * opposed to an array (1), a function (2), a typed-array view (3) or an
 * ArrayBuffer (4). All of them reach bronze_prop_get, so the fast path has
 * to discriminate on this before it believes anything else. */
#define BRONZE_ABI_OBJ_FLAGS_OFFSET      2
#define BRONZE_ABI_OBJ_FLAGS_PLAIN       0

/* ObjectHeader: the shape word, then the out-of-line overflow Value, then
 * kInlineSlots inline Values. Slots at or past kInlineSlots live in the
 * overflow block and are NOT covered by the inline fast path. */
#define BRONZE_ABI_OBJ_SHAPE_OFFSET      8
#define BRONZE_ABI_OBJ_SLOTS_OFFSET     24
#define BRONZE_ABI_OBJ_INLINE_SLOTS      4

/* Every Object-tagged heap allocation has at least this many payload bytes,
 * which is what makes the fast path's unconditional load of the shape word
 * (offset 8..15) safe BEFORE the flags discrimination has passed. Pinned by
 * static_asserts over every Object-tagged header in rt_helpers.cpp. */
#define BRONZE_ABI_OBJ_MIN_PAYLOAD       8

/*
 * A generated function's GC root frame: allocated in the
 * function's own stack frame, linked onto bronze_gc_frame_top on entry and
 * unlinked before every return, so the collector can find every Dynamic
 * value compiled code is holding. Generated code links and unlinks inline
 * — no helper call — because the call-heavy path pays this per invocation.
 *
 * `slots` is `count` entries long, inline after the header. Deliberately
 * NOT thread-local: bronze has no threads and no design for them (0006).
 */
typedef struct bronze_gc_frame {
    struct bronze_gc_frame* prev;
    uint64_t count;
    uint64_t slots[1];
} bronze_gc_frame;

/*
 * The pending exception. `bronze_exception_cell` holds
 * the thrown value, or BRONZE_ABI_NO_EXCEPTION_BITS when nothing is pending.
 * Generated code, after every instruction that can throw, loads it, compares
 * it against that constant and branches — no helper call, for the same reason
 * the GC frame is linked inline: this is on the call-heavy path.
 *
 * Two rules the runtime side must keep, because nothing enforces them:
 *
 *  - a helper that sets the cell RETURNS BRONZE_ABI_UNDEFINED_BITS. Its
 *    caller stores the result into a GC root slot before it tests the cell,
 *    and the collector reads every slot of a linked frame.
 *  - the cell is a permanent GC root. A thrown object is live for exactly as
 *    long as it is pending, across an arbitrary number of frames, and nothing
 *    else roots it.
 *
 * There is no unwind ABI beyond this: propagation is an ordinary `ret`, so
 * the frame pop before it is the one the non-throwing path already emits.
 */

/* C-type expansion of the tokens, scoped to the prototype block below and
 * #undef'd after, so consumers can rebind the tokens (codegen-llvm binds
 * them to llvm::Type*). */
#define BRONZE_ABI_U64    uint64_t
#define BRONZE_ABI_U32    uint32_t
#define BRONZE_ABI_I32    int32_t
#define BRONZE_ABI_F64    double
#define BRONZE_ABI_BOOL   bool
#define BRONZE_ABI_CSTR   const char*
#define BRONZE_ABI_PU64   const uint64_t*
#define BRONZE_ABI_MU64   uint64_t*
#define BRONZE_ABI_FRAMEPTR bronze_gc_frame*
#define BRONZE_ABI_FNPTR  bronze_fn_code
#define BRONZE_ABI_VOID   void
#define BRONZE_ABI_NOARGS void

#define BRONZE_ABI_DECLARE(name, RET, PARAMS) RET name PARAMS;
BRONZE_ABI_FUNCTIONS(BRONZE_ABI_DECLARE)
#undef BRONZE_ABI_DECLARE

#define BRONZE_ABI_DECLARE_GLOBAL(name, TYPE) extern TYPE name;
BRONZE_ABI_GLOBALS(BRONZE_ABI_DECLARE_GLOBAL)
#undef BRONZE_ABI_DECLARE_GLOBAL

#undef BRONZE_ABI_U64
#undef BRONZE_ABI_U32
#undef BRONZE_ABI_I32
#undef BRONZE_ABI_F64
#undef BRONZE_ABI_BOOL
#undef BRONZE_ABI_CSTR
#undef BRONZE_ABI_PU64
#undef BRONZE_ABI_MU64
#undef BRONZE_ABI_FRAMEPTR
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS

#ifdef __cplusplus
}
#endif

#endif /* BRONZE_ABI_H */
