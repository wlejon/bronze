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
 *
 * Drift between two BUILDS is a different failure, and it has its own
 * tripwire: the build hashes this file into BRONZE_ABI_FINGERPRINT
 * (src/abi/CMakeLists.txt), codegen stamps that value into every emitted
 * object as `const uint32_t bronze_object_abi_fingerprint`, and both
 * program entries (src/rt/rt.cpp's main, embed's runMain) compare the
 * object's stamp against the runtime's own value before bronze_main runs
 * (runtime/abi_guard.h). An object compiled by a bronze whose ABI differs
 * from the runtime it is linked with therefore dies at startup with both
 * values named — or at link, for an object old enough to carry no stamp —
 * instead of reading garbage through a drifted signature. (The motivating
 * failure: a host adopted a stale object after a helper grew a parameter,
 * and the runtime read the missing argument as stack garbage — not a
 * crash, but half-minute stalls at nondeterministic points.) Editing this
 * header is what moves the version; there is no number to forget.
 */

/*
 * ---- the loadable-module surface ----------------------------------------
 *
 * A compiled module is not only an object for a host's own link step: with
 * `--emit-shared` it is a DLL/.so/.dylib a host LOADS at run time, and a
 * loader that cannot see the host's build has to learn everything it needs
 * from symbols. There are exactly four, all named after the module's entry
 * (`--entry-symbol`, default `bronze_main`), and this is the whole contract:
 *
 *   <entry>                    void(void) — the module's top level.
 *   <entry>_abi_fingerprint    const uint32_t — the stamp described above.
 *                              Spelled `bronze_object_abi_fingerprint` for
 *                              the default entry, which is the historical
 *                              name src/rt/rt.cpp and embed_run.cpp link.
 *   <entry>_host_globals       const, 4-byte aligned:
 *
 *                                  uint32_t count;
 *                                  char     names[];  // `count` NUL-terminated
 *                                                     // UTF-8 names, back to back
 *
 *   <entry>_native_imports     WRITABLE, 8-byte aligned — the module's
 *                              native import table:
 *
 *                                  uint32_t count;
 *                                  uint32_t namesOffset; // from the table's first byte
 *                                  uint64_t slots[count];
 *                                  char     names[];     // at namesOffset: `count`
 *                                                        // pairs of NUL-terminated
 *                                                        // "name", "signature"
 *
 *                              One slot per native the module was compiled
 *                              to call directly (`--native-manifest`), each
 *                              named "<kind> <path>" — `function
 *                              bro.mesh.box`, `method bro.ai.AIAgent.move`,
 *                              `constructor bro.ai.AIAgent`, `getter
 *                              bro.time.scale`, `setter bro.time.scale` — with
 *                              the canonical signature text beside it, or
 *                              "class <path>" with an empty signature for a
 *                              slot that holds the class's tag. Every call
 *                              site loads its slot and calls through it. The
 *                              slots start as the address of
 *                              bronze_native_unbound (class slots as 0) and
 *                              are filled by name from the host's registry —
 *                              by embed::bindNativeImports if the loader
 *                              asks, and by the entry's own first call
 *                              (bronze_native_bind) regardless. A module
 *                              compiled against no natives defines the table
 *                              with count 0. The symbol names the image's
 *                              own table, which is its HOME thread's (the
 *                              first to run the entry): each other thread
 *                              runs on a per-thread copy of the module's
 *                              writable data (bronze_module_instance) and
 *                              its entry binds that copy for itself.
 *
 * The manifest is what the module was compiled against — the `--host-globals`
 * list, verbatim and in the order the manifest gave it. It exists because
 * `--host-globals` is a CONTRACT between two builds: a name in it compiled
 * into a plain provided-global read, so a host that forgets to register that
 * name hands the program a global read with nothing behind it. A loader diffs
 * this list against what it has registered and refuses by name, before the
 * entry runs, instead of failing somewhere inside the program's top level.
 *
 * A module compiled with no manifest still DEFINES the symbol, with count 0.
 * "No manifest" and "not a bronze module" must not be the same observation,
 * and the absence of a symbol cannot tell them apart.
 *
 * Primitives only, exactly as the registry below is: a count and bytes. The
 * loader is not necessarily C++, and even when it is, the sret rule the top
 * of this header states applies to everything a module exports.
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
#define BRONZE_ABI_FALSE_BITS     0xFFF4000000000000ull
#define BRONZE_ABI_TRUE_BITS      0xFFF4000000000001ull

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
/* "there is no interned key index for this read" — what a COMPUTED read passes
 * where a named one passes its site's key id. Runtime-side callers only:
 * no instruction is ever emitted with it. */
#define BRONZE_ABI_KEY_NONE 0xFFFFFFFFu

/* What the SYNTAX of a function decided about the object, as one byte the
 * compiler hands `bronze_create_function` and `bronze_function_singleton`.
 *
 * Two of ECMA-262 10.2's answers cannot be recovered from anything the runtime
 * can see — an arrow, a method and a plain function expression produce the same
 * code pointer, the same parameters and the same body — and both are
 * observable: `new (() => {})` is a TypeError (10.2.2 gives a non-constructor
 * no [[Construct]]), and `({ m(){} }).m.prototype` is `undefined` (15.4.4
 * defines a MethodDefinition without one). So they travel from the parser
 * rather than being guessed at the allocation.
 *
 * GENERATOR and ASYNC ride in the same byte because they are the same kind of
 * fact and are wanted in the same places: `Object.prototype.toString` needs
 * them to answer "[object AsyncGeneratorFunction]", and a generator has a
 * `prototype` while being no constructor at all, which no single bit could say.
 *
 * ORDINARY is the default a function object created with no flags gets — every
 * native builtin, which is a C function pointer with no syntax behind it — and
 * it is today's behaviour spelled out rather than a new one. */
#define BRONZE_ABI_FN_FLAG_CONSTRUCT   0x01u
#define BRONZE_ABI_FN_FLAG_PROTOTYPE   0x02u
#define BRONZE_ABI_FN_FLAG_GENERATOR   0x04u
#define BRONZE_ABI_FN_FLAG_ASYNC       0x08u
#define BRONZE_ABI_FN_FLAG_CLASS_CTOR  0x10u
/* Not compiled from source text: a runtime builtin, a bound function, or a
 * function the host registered. It is the bit `Function.prototype.toString`
 * asks — 20.2.3.5 gives exactly these the NativeFunction string, and every
 * other function object its own source — and generated code never sets it,
 * because a function bronze compiled is by construction not one of these. */
#define BRONZE_ABI_FN_FLAG_NATIVE      0x20u
/* The function requires its closure environment record to be passed on call.
 * When false, the wrapper ignores `env` and passing `undefined` is sound. */
#define BRONZE_ABI_FN_FLAG_NEEDS_ENV   0x40u
#define BRONZE_ABI_FN_FLAGS_ORDINARY (BRONZE_ABI_FN_FLAG_CONSTRUCT | BRONZE_ABI_FN_FLAG_PROTOTYPE)

/* The uninitialized-binding singleton (runtime/value.h Tag::Uninitialized):
 * what an environment slot holds for a `let`, `const` or `class` binding
 * between the moment its scope is entered and the moment its declaration is
 * evaluated. Generated code materializes it directly — `env.init.tdz` is a
 * plain `bronze_env_set` of this constant — so no helper exists to produce
 * it, and no helper ever returns it. Pinned against the runtime's Value
 * constructor by a static_assert in rt_object.cpp. */
#define BRONZE_ABI_UNINITIALIZED_BITS 0xFFFA000000000000ull

/* A property descriptor that a compile-time literal already settled, as the
 * mask `bronze_define_own_attr` takes in place of the object 6.2.6.5 would
 * have decoded.
 *
 * Each of the four data fields needs TWO bits and not one, because 10.1.6.3
 * distinguishes a field the descriptor OMITS from a field it sets to false: an
 * omitted `writable` leaves an existing property's writability alone and
 * defaults to false only on a new one, so "absent" and "present and false" are
 * different programs. HAS says the literal wrote the field; the second bit
 * carries what it wrote.
 *
 * `get` and `set` have no bits: an accessor descriptor is not lowered this
 * way, and a mask that could not spell one is what makes that refusal
 * checkable here rather than only at the call site. */
#define BRONZE_ABI_DESC_HAS_VALUE        0x01u
#define BRONZE_ABI_DESC_HAS_WRITABLE     0x02u
#define BRONZE_ABI_DESC_WRITABLE         0x04u
#define BRONZE_ABI_DESC_HAS_ENUMERABLE   0x08u
#define BRONZE_ABI_DESC_ENUMERABLE       0x10u
#define BRONZE_ABI_DESC_HAS_CONFIGURABLE 0x20u
#define BRONZE_ABI_DESC_CONFIGURABLE     0x40u

/* COROUTINE BODIES. A generator, an async function and an async generator
 * each compile to a body that runs on a brass coroutine frame, and to a stub
 * that `bronze_coro_start`s it with one of these kinds.
 *
 * A body suspends with one of the SUSPEND kinds, which the backend puts in
 * the low two bits of the suspension's brass state id so that the resumer
 * reads it off the frame header: START ends a generator's argument binding
 * (15.5.3 runs it at the call, the body at the first `next`), YIELD hands a
 * value out, AWAIT one to await, and DELEGATE hands out an inner iterator's
 * result object as it is (a `yield*`, 27.5.3.8).
 *
 * A resumption continues the body in one of brass's CoroResumeMode modes. The
 * resumer passes the ADDRESS of a rooted cell holding the sent value, and the
 * body reads the value through it: the cell is updated when a collection
 * moves what it names, and the frame's own resume word would not be. */
#define BRONZE_ABI_CORO_GENERATOR       0u
#define BRONZE_ABI_CORO_ASYNC           1u
#define BRONZE_ABI_CORO_ASYNC_GENERATOR 2u
#define BRONZE_ABI_SUSPEND_START        0u
#define BRONZE_ABI_SUSPEND_YIELD        1u
#define BRONZE_ABI_SUSPEND_AWAIT        2u
#define BRONZE_ABI_SUSPEND_DELEGATE     3u
#define BRONZE_ABI_SUSPEND_KIND_MASK    3u
#define BRONZE_ABI_RESUME_NEXT          0u
#define BRONZE_ABI_RESUME_THROW         1u
#define BRONZE_ABI_RESUME_RETURN        2u

#include "abi/bronze_abi_functions.h"
#include "abi/bronze_abi_layout.h"
enum {
    BRONZE_FN_DESC_METHOD      = 1u << 0,
    BRONZE_FN_DESC_CONSTRUCTOR = 1u << 1,
    BRONZE_FN_DESC_ASYNC       = 1u << 2,
    BRONZE_FN_DESC_TOPLEVEL    = 1u << 3,
    BRONZE_FN_DESC_BUILTIN     = 1u << 4,
};

typedef struct bronze_fn_desc {
    const char* name;
    const char* file;
    uint32_t def_line;
    uint32_t def_col;
    uint32_t flags;
    uint32_t reserved;
    const void* code;
} bronze_fn_desc;

/*
 * One source position in a compiled function's pc table. `file` indexes the
 * range's `files` (the compiled module's source files): a function's code
 * can come from more than one file — a program's merged top level runs every
 * imported module's top-level statements — so the file is per position, not
 * the descriptor's. BRONZE_PC_FILE_DESC (or an index past `file_count`)
 * means the descriptor's own file.
 */
#define BRONZE_PC_FILE_DESC 0xFFFFFFFFu

typedef struct bronze_pc_entry {
    uint32_t pc_offset;
    uint32_t line;
    uint32_t col;
    uint32_t file;
} bronze_pc_entry;

typedef struct bronze_code_range {
    const void* code_start;
    uint32_t code_size;
    uint32_t pc_count;
    const bronze_fn_desc* desc;
    const bronze_pc_entry* pc_table;
    const char* const* files;
    uint32_t file_count;
    /* The function's brass stack map, encoded (brass encode_stack_maps, one
     * function whose code starts at `code_start`): the frames of this code
     * the collector walks, and where each holds a Value. Emitted into every
     * compiled object; a loaded image's registration hands it to brass's code
     * registry (bronze_register_code_ranges) unless the code's maps are
     * registered already, as a JIT's are. Null when the function has none. */
    uint32_t stack_map_size;
    const uint8_t* stack_map;
} bronze_code_range;

/*
 * The per-thread ABI data block: every mutable word generated code shares
 * with the runtime, one instance per OS thread, fetched once per compiled
 * function through bronze_tls_block_addr() and read or written at fixed byte
 * offsets from that base. One block rather than one thread_local per word
 * because Windows cannot import a thread_local across a DLL boundary, so
 * per-thread data costs a call — and one call covering every word is the
 * cheapest that call gets.
 *
 * The layout is ABI: the BRONZE_TLS_*_OFF constants below are what codegen
 * emits, the runtime static_asserts them against this struct (tls_block.cpp),
 * and any change here moves the fingerprint. Field order is by heat.
 *
 * No exception state lives here. A throw is a native raise (brass's
 * zero-cost EH): generated code calls inside a `try` as `invoke` with a
 * landing pad, and a runtime helper throws a C++
 * brass::runtime::BrassException, which the same unwinder carries through
 * compiled frames (runtime/exception.h).
 *
 * The fields:
 *
 *  - proto_epoch: the prototype-mutation epoch (runtime/object.h): what
 *    makes a cached depth > 0 property hit sound, read inline by the
 *    proto-hit fast path.
 *
 *  - alloc_cursor / alloc_limit: the inline-allocation window. They ARE the
 *    thread heap's young bump region: the collector (brass::gc::Heap, bound
 *    to these two words by runtime/heap.cpp) allocates from them itself and
 *    resets them after each collection. Generated code bump-allocates plain
 *    objects, arrays and environments from [cursor, limit), writing each
 *    one's collector header (gc_cell_header | size) in the word before the
 *    bronze header. The inline path only ever ADVANCES cursor when the
 *    object fits — it can never collect — and every miss falls back to the
 *    allocating helper, which collects when it must. Under a stress mode the
 *    collector keeps the window empty so every allocation reaches it. Zero/
 *    zero is the initial and the BRONZE_NO_INLINE_ALLOC=1 state: the
 *    unsigned subtraction limit-cursor is then 0, no size fits, and the fast
 *    path is dormant.
 *
 *  - plain_shape: the plain object root shape pointer for inline object
 *    creation.
 *
 *  - inline_call_enabled / array_method_ic_enabled /
 *    inline_overflow_set_enabled / inline_accessor_enabled / poly_ic_enabled
 *    / negative_ic_enabled: the inline fast-path enable flags, 1 by default,
 *    each set to 0 per thread under its BRONZE_NO_* environment variable
 *    (BRONZE_NO_INLINE_CALL, BRONZE_NO_ARRAY_METHOD_IC,
 *    BRONZE_NO_INLINE_OVERFLOW_SET, BRONZE_NO_INLINE_ACCESSOR,
 *    BRONZE_NO_POLY_IC, BRONZE_NO_NEG_IC) so one binary can A/B test each
 *    inline path against its helper.
 */

#include "abi/bronze_abi_tls.h"

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
#define BRONZE_ABI_PU32   const uint32_t*
#define BRONZE_ABI_PU8    const uint8_t*
#define BRONZE_ABI_MU64   uint64_t*
#define BRONZE_ABI_MU32   uint32_t*
#define BRONZE_ABI_TLSPTR bronze_tls_block*
#define BRONZE_ABI_FNPTR  bronze_fn_code
#define BRONZE_ABI_VPTR   void*
#define BRONZE_ABI_CVPTR  const void*
#define BRONZE_ABI_VOID   void
#define BRONZE_ABI_NOARGS void

#define BRONZE_ABI_DECLARE(name, RET, PARAMS) RET name PARAMS;
BRONZE_ABI_FUNCTIONS(BRONZE_ABI_DECLARE)
#undef BRONZE_ABI_DECLARE

#undef BRONZE_ABI_U64
#undef BRONZE_ABI_U32
#undef BRONZE_ABI_I32
#undef BRONZE_ABI_F64
#undef BRONZE_ABI_BOOL
#undef BRONZE_ABI_CSTR
#undef BRONZE_ABI_PU64
#undef BRONZE_ABI_PU32
#undef BRONZE_ABI_PU8
#undef BRONZE_ABI_MU64
#undef BRONZE_ABI_MU32
#undef BRONZE_ABI_TLSPTR
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VPTR
#undef BRONZE_ABI_CVPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS

#ifdef __cplusplus
}
#endif

#endif /* BRONZE_ABI_H */
