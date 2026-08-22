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
 * from symbols. There are exactly three, all named after the module's entry
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

/* The Hole singleton, which is what the TLS block's `exception_cell` holds when no
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

/*
 * X(name, RET, PARAMS)
 *   RET    — one BRONZE_ABI_* type token (BRONZE_ABI_VOID for none)
 *   PARAMS — parenthesized comma list of type tokens;
 *            (BRONZE_ABI_NOARGS) for an empty parameter list
 */
#define BRONZE_ABI_FUNCTIONS(X) \
    /* The calling thread's ABI data block (bronze_tls_block below): the ONE\
     * data surface generated code shares with the runtime. A function call\
     * rather than data symbols because Windows cannot import a thread_local\
     * across a DLL boundary — cross-image TLS is reachable only through a\
     * call — and per-thread is the point: each compiled function fetches its\
     * thread's block once in its prologue and reaches every field by fixed\
     * offset from that base. */ \
    X(bronze_tls_block_addr,      BRONZE_ABI_TLSPTR, (BRONZE_ABI_NOARGS)) \
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
    X(bronze_to_uint8_clamp_f64,  BRONZE_ABI_I32,  (BRONZE_ABI_F64)) \
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
    X(bronze_create_async_generator_object, BRONZE_ABI_U64, (BRONZE_ABI_U64)) \
    X(bronze_async_machine,       BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_start,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_await,         BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_async_iter_open,     BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_iter_next,     BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_iter_close,    BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_dynamic_import,      BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_module_namespace,    BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_object_keys,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_for_in_keys,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_method_def,          BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_method_def_computed, BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_accessor_def,        BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_accessor_def_computed, BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_get_new_target,      BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    X(bronze_import_meta,         BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_super_call,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_super_call_spread,   BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_template_object,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_MU64)) \
    X(bronze_array_append_hole,   BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_prop_delete,         BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_BOOL)) \
    X(bronze_elem_delete,         BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    /* A provided global by interned key id, plus the CALLING MODULE's own cache
     * cell for that global (null from the runtime's own callers, which have no
     * module). The helper owns the decision to fill: only a builtin resolution
     * is written back, so a host-registered name and a `globalThis.x` the
     * program can reassign keep their scan-per-read semantics by never
     * reaching a cell. */ \
    X(bronze_global_get,          BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_MU64)) \
    X(bronze_resolve_name,        BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_BOOL)) \
    X(bronze_immutable_assign,    BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    X(bronze_arguments_object,    BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_arg_at,              BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_U32)) \
    X(bronze_class_extends,       BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    /* Private class elements (6.2.12). One TABLE per private name per class\
     * evaluation, keyed by the object that carries the element: `_new` mints\
     * one, `_add` installs an element (which is what establishes the brand),\
     * `_has` is `#x in o`, and `_get`/`_set` are the two accesses that\
     * require the brand and name the private name in the TypeError when it is\
     * absent. `_misuse` raises the three TypeErrors a well-branded access can\
     * still be — writing a method, reading a set-only accessor, writing a\
     * get-only one — which lowering knows at compile time. The u32 in each is\
     * a registered key index holding the private name's text. */ \
    X(bronze_private_new,         BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    X(bronze_private_has,         BRONZE_ABI_BOOL, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_private_get,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_private_add,         BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_private_set,         BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_private_misuse,      BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_create_array,        BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_create_function,     BRONZE_ABI_U64,  (BRONZE_ABI_FNPTR, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_env_create,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_env_get,             BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_env_get_tdz,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_env_set,             BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_prop_get,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_MU64)) \
    X(bronze_super_get,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_super_set,           BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_prop_set,            BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_MU64, BRONZE_ABI_BOOL)) \
    X(bronze_static_shape_publish,BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_MU64, BRONZE_ABI_U32, BRONZE_ABI_BOOL)) \
    /* The LAYOUT-FAMILY stamp: what lets one site serve every subclass of the
     * class whose method it was written in. `bronze_static_shape_publish`
     * above pins ONE shape, which is right for a receiver that has one, and
     * permanently wrong for `this` inside a base-class method — three.js never
     * constructs a bare Object3D, so `this.matrixWorld` there runs on a Group,
     * a Mesh and a Scene, three shapes with Object3D's fields at the same
     * slots.
     *
     * This helper stamps a SHAPE with the id of the most specific registered
     * class whose whole declared field list is a genuine prefix of that
     * shape's own properties — checked name by name, slot by slot, attribute
     * by attribute, against the shape the object actually has. The compiler
     * numbers its classes in PREORDER over the `extends` forest, so a class's
     * descendants occupy a contiguous id range and a site's guard is a load of
     * the stamp and one unsigned range compare. Ids are module-relative and
     * biased by the base the module was handed at registration, which is what
     * keeps two modules' class 3 apart.
     *
     * Nothing upstream has to be sound for this to be correct: the stamp is a
     * fact the runtime verified about the shape in front of it, so a wrong
     * layout claim costs a guard that never matches. Called once per shape,
     * from a site's slow path, and only while the stamp is still zero. */ \
    X(bronze_family_stamp,        BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
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
    X(bronze_array_push,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_array_spread,        BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_object_spread,       BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_object_rest,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_call_spread, BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_spread,    BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_elem_set,            BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_dynamic_call,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_construct,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    /* The one function object for a mention of a declaration. The last argument
     * is the calling module's own {code, value} cache slot for this mention, or
     * null: the runtime's native-builtin interning has no module to hold a slot
     * in, and passing null keeps it on the by-code-pointer map, which is the
     * authority either way. */ \
    X(bronze_function_singleton,        BRONZE_ABI_U64,  (BRONZE_ABI_FNPTR, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_MU64)) \
    X(bronze_string_concat,             BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_add,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    /* The rest of 13.15.3 ApplyStringOrNumericBinaryOperator over BOXED\
     * operands, which `+` alone used to need. They exist because a BigInt\
     * operand makes every one of these a two-algorithm operator: ToNumeric\
     * (7.1.3) answers with a Number or a BigInt, the two must MATCH, and a\
     * mixed pair is a TypeError rather than a coercion. The number/number\
     * case is still inlined at the call site (llvm_arith.cpp), so these are\
     * the off-the-fast-path half and nothing typed code reaches. */ \
    X(bronze_dynamic_sub,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_mul,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_div,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_mod,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_pow,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_bitand,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_bitor,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_bitxor,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_shl,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_shr,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_ushr,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_dynamic_neg,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_dynamic_bitnot,      BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    /* A BigInt literal: the registered key index of its SOURCE TEXT in, the\
     * value out. The text rather than a payload because a BigInt has no\
     * width - there is no immediate field it would fit in - and the key pool\
     * already carries compile-time strings to the runtime. */ \
    X(bronze_bigint_literal,      BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    /* 7.1.3 ToNumeric and 13.4.4.1 step 3, the two halves of `x++`. They are\
     * two helpers and not one because a POSTFIX update yields the coerced OLD\
     * value, so the coercion is observable on its own. The step is an operator\
     * rather than `x + 1` because its delta has the operand's type: 1 for a\
     * Number and 1n for a BigInt, and the mixed pair would be a TypeError. */ \
    X(bronze_to_numeric,          BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_numeric_step,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_print_value,         BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_values,        BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_print_string,        BRONZE_ABI_VOID, (BRONZE_ABI_CSTR)) \
    X(bronze_print_value_err,     BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_values_err,    BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_print_spread,        BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_spread_err,    BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    /* INTERNS a compile-time key string and answers its process-wide id. Two
     * modules that both mention "position" get the same id, which is what makes
     * shapes, inline caches and property identity mean the same thing on both
     * sides of a module boundary. A module numbers its own keys 0..n-1 and
     * stores the answers in a module-local remap array, so the number in its
     * instruction stream stays an immediate and only the value handed to a
     * helper is process-wide. */ \
    X(bronze_register_key_string, BRONZE_ABI_U32,  (BRONZE_ABI_CSTR)) \
    /* A module's own root spans, handed over at module init. Both hold Values
     * in the module's .data/.bss, and the collector forwards them in place,
     * which is what lets generated code read a cell through a compile-time
     * constant address and still see current bits after a collection.
     *
     * Registration-only, on purpose: generated code never unregisters. The
     * unregister half is a HOST seam (embed.h's beginModuleLoad/unloadModule
     * bracket an entry and can later drop everything it registered), because
     * only the host knows when a module's life ends — and because unload is
     * only sound under the host-side contract stated there (the image is
     * never freed).
     *
     * `bronze_register_value_cells` takes `count` plain Value cells: a module
     * calls it for its global cache and for its module-environment cell, which
     * are the two places its own data holds a Value outright.
     *
     * `bronze_register_fn_slots` takes `count` {code, value} pairs
     * (BRONZE_ABI_FNSLOT_SIZE bytes each), of which only the value word is a
     * Value — the code word is a raw function pointer the collector must not
     * touch, and a null one means the slot was never filled. */ \
    X(bronze_register_value_cells, BRONZE_ABI_VOID, (BRONZE_ABI_MU64, BRONZE_ABI_U64)) \
    X(bronze_register_fn_slots,    BRONZE_ABI_VOID, (BRONZE_ABI_MU64, BRONZE_ABI_U64)) \
    /* The module's METHOD-CALL sites, handed over at module init: `siteIndexes`
     * is `count` u64 site numbers into the module's IC table (`icTable` is its
     * base). Word BRONZE_ABI_METHOD_IC_ENV_WORD of each named site is the env
     * argument a latched direct-form hit passes verbatim, and a latch may put a
     * closure's environment record there — a HEAP Value in module .bss, which
     * only registration as an ordinary value cell keeps current across a
     * collection. The runtime registers exactly those words, one cell each, so
     * the collector forwards them in place under the same module epoch as the
     * spans above; an unregistered env word would dangle at the first flip,
     * which is why the latch never installs an env-carrying entry into a table
     * whose module did not make this call (fingerprint pairing guarantees it
     * did). Registration-only, like the value-cell spans it rides on. */ \
    X(bronze_register_method_ic_cells, BRONZE_ABI_VOID, (BRONZE_ABI_MU64, BRONZE_ABI_PU64, BRONZE_ABI_U64)) \
    /* The module's proven class LAYOUTS, handed over at module init so that
     * `bronze_family_stamp` can recognise a shape as an instance of one.
     *
     * `classes` is `classCount` pairs of u32 — a field start and a field count
     * — indexing `fields`, which holds one u32 per field: the module's own key
     * index shifted left one, with bit 0 set when the construction sequence
     * installs that field WRITABLE. (`Object.defineProperty(this, 'id', {value:
     * n})` installs a non-writable one, and three.js roots four `extends`
     * chains that way, so the bit is not decoration: a write site may only
     * claim a slot the stamp proved writable.) `keyMap` is the module's own
     * key-index -> process-wide-id array, already filled by the
     * `bronze_register_key_string` loop that precedes this call.
     *
     * The classes arrive in PREORDER over the `extends` forest, so ids are
     * contiguous per subtree; the runtime allocates `classCount` consecutive
     * ids from a process-wide counter and writes the first one into
     * `*baseCell`, which is the one word every family guard in the module
     * loads. Registration-only, like the two above. */ \
    X(bronze_register_class_family, BRONZE_ABI_VOID, \
      (BRONZE_ABI_PU32, BRONZE_ABI_U32, BRONZE_ABI_PU32, BRONZE_ABI_PU32, BRONZE_ABI_MU64)) \
    /* One source FILE and the functions written in it, handed over at module
     * init so that 20.2.3.5 Function.prototype.toString can return the source
     * text the spec says it returns rather than "[native code]".
     *
     * `text`/`textLen` are the file's bytes, in the object file's read-only
     * data; `entries` is `count` PAIRS of u64 — a call-wrapper address, then
     * the byte range packed as `(begin << 32) | (end - begin)`. Pairs rather
     * than a struct because the registry carries primitives only, and the
     * wrapper address rather than any per-function object because the address
     * is the one identity every closure over that body shares: a thousand
     * closures created in a loop register nothing and cost nothing, which is
     * the whole reason the table is keyed this way.
     *
     * Registration-only for the same reason the two above are. */ \
    X(bronze_register_fn_sources,  BRONZE_ABI_VOID, (BRONZE_ABI_CSTR, BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_U32)) \
    X(bronze_uncaught_exception,  BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    /* The six Math members generated code can dispatch directly: exported so a
     * call site can compare a callee's FunctionHeader::code against the symbol
     * — the code pointer is the one identity a GC that moves the function
     * OBJECT can never disturb, and comparing it is what keeps
     * `Math.sqrt = f` honest: an overwritten member has a different code
     * pointer and the site falls back to bronze_dynamic_call. The first six
     * are the function objects' own code (bronze_fn_code-shaped); the last
     * four are the scalar kernels the inline fast path calls, each the SAME C
     * runtime function the helper path runs, so the two paths cannot differ
     * by a bit (the determinism rule: llvm.sqrt/llvm.fabs are IEEE-exact and
     * inlined; sin/cos/min/max are not and stay C calls). */ \
    X(bronze_math_sqrt,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_sin,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_cos,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_abs,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_min,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_max,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_sin_f64,        BRONZE_ABI_F64,  (BRONZE_ABI_F64)) \
    X(bronze_math_cos_f64,        BRONZE_ABI_F64,  (BRONZE_ABI_F64)) \
    X(bronze_math_min2_f64,       BRONZE_ABI_F64,  (BRONZE_ABI_F64, BRONZE_ABI_F64)) \
    X(bronze_math_max2_f64,       BRONZE_ABI_F64,  (BRONZE_ABI_F64, BRONZE_ABI_F64)) \
    /* Method call IC runtime helpers: property lookup + IC update + dispatch. */ \
    X(bronze_call_method,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_MU64)) \
    X(bronze_call_method_spread,  BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_MU64))

/*
 * There are no data symbols in this ABI. Every mutable word generated code
 * shares with the runtime lives in the per-thread `bronze_tls_block` defined
 * after the GC-frame layout below, reached through bronze_tls_block_addr()
 * (first entry in the registry above).
 *
 * The provided-globals cache and the function-singleton slot cache are not
 * in the block either: they are arrays in the MODULE's own data, sized at
 * compile time and registered with the runtime at module init
 * (bronze_register_value_cells / bronze_register_fn_slots above). Two
 * compiled modules in one process is why — a runtime-owned table indexed by
 * module-assigned numbers has exactly one owner, and a second module's
 * index 7 is not the first's. Module-owned is also the cheaper shape: the
 * length is a compile-time fact, so the bounds check and the table-pointer
 * load both disappear and the cell is a constant address.
 */

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
 * because that add changes only the intermediate's shape.
 *
 * ---- a SITE is BRONZE_ABI_IC_WAYS entries ---------------------------------
 *
 * The table's stride is a SITE, not an entry: BRONZE_ABI_IC_WAYS entries laid
 * out one after another, way 0 first. A site's way 0 is byte-identical to
 * what a whole site used to be, which is why every runtime path that takes an
 * `InlineCache*` still works when handed the site pointer generated code and
 * the helpers pass around.
 *
 * READ sites use every way; a WRITE site uses way 0 only (a write's bill is
 * transitions, not shape variety — bench/README.md's chunk 14 measured it).
 * The unused ways of a write site cost BSS and nothing else.
 *
 * Generated code compares the receiver's shape against way 0, then — only on
 * a way-0 miss, and only while `poly_ic_enabled` — against ways 1..N-1, and
 * the matched way's entry pointer is what the rest of the fast path reads.
 * The helper installs with move-to-front: a fresh entry lands at way 0 and
 * pushes the others down, so the last shape to miss is the first one checked
 * and the least recently installed falls off the end. No cursor word is
 * needed, which is why the site is exactly N entries wide.
 *
 * ---- the ABSENT (negative) entry ------------------------------------------
 *
 * `cached_depth == BRONZE_ABI_IC_DEPTH_ABSENT_FLAG` means: this key is on
 * NEITHER the receiver nor any link of its prototype chain, so the read
 * answers `undefined` with no walk at all. `cached_slot` is unused and 0.
 *
 * Its validity is the depth > 0 entry's, exactly: the receiver's shape covers
 * every own-property add (an add transitions the shape), and the epoch covers
 * every way a key can appear on the chain — an add to any marked-prototype
 * shape, a dictionary define, a prototype swap. The fill additionally proves
 * the chain runs to its END through plain, non-dictionary objects whose
 * shapes are all MARKED as prototypes, because an unmarked link is one whose
 * adds would not bump the epoch (runtime/object.cpp, absentThroughChain).
 *
 * The flag is 0x40000000 rather than a shape sentinel because a negative
 * entry still names a real shape to compare against, and because it must not
 * collide with the accessor flag in the same field. Real depths are bounded
 * by ObjectHeader::kMaxPrototypeDepth, far below either bit.
 *
 * Non-shape sentinel discipline:
 * When an entry caches an Array built-in method (or the Array constructor),
 * `cached_shape` holds BRONZE_ABI_IC_SHAPE_ARRAY_METHOD ((uintptr_t)1).
 * Real Shape pointers are 8-byte aligned arena allocations and can never be 1.
 * The slot word then holds the method ID — an index into
 * the TLS block's `array_method_tbl`, NOT a Value: the collector moves function
 * objects, so the Value lives in that rooted table and the entry stores
 * only the immortal index. The epoch word is 0 and unread for these
 * entries. Only GET sites ever hold the sentinel (lowering never shares
 * an IC index between a read and a write, `lower_update.cpp`), which is
 * why the set-side transition arm may still dereference `cached_shape`
 * after its null check.
 */
#define BRONZE_ABI_IC_ENTRY_SIZE     24 /* sizeof(InlineCache) */
#define BRONZE_ABI_IC_SHAPE_OFFSET    0 /* InlineCache::cached_shape (pointer) */
#define BRONZE_ABI_IC_SLOT_OFFSET     8 /* InlineCache::cached_slot  (uint32) */
#define BRONZE_ABI_IC_DEPTH_OFFSET   12 /* InlineCache::cached_depth (uint32) */
#define BRONZE_ABI_IC_EPOCH_OFFSET   16 /* InlineCache::cached_epoch (uint64) */
#define BRONZE_ABI_IC_SHAPE_ARRAY_METHOD 1ull
#define BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG 0x80000000u
#define BRONZE_ABI_IC_DEPTH_ABSENT_FLAG   0x40000000u
/* How many (shape -> answer) entries one property site holds, and the stride
 * the IC table is therefore indexed by. Four, from the marker-probe evidence
 * in brobench/analysis/chunk1_bill.md: three.js's polymorphic sites mix an
 * Object3D/Mesh pair with a Scene and a light or two, and four ways is the
 * width that holds that mix while keeping the way scan a short compare chain
 * a way-0 hit never enters. */
#define BRONZE_ABI_IC_WAYS 4
#define BRONZE_ABI_IC_SITE_SIZE (BRONZE_ABI_IC_ENTRY_SIZE * BRONZE_ABI_IC_WAYS)
/* slot and depth are adjacent and little-endian, so the single u64 at
 * IC_SLOT_OFFSET is (depth << 32) | slot. `that word < kInlineSlots` is
 * therefore ONE compare meaning "own property, in an inline slot" — the
 * exact envelope the inline fast path covers. Reading only the low half
 * would forget the depth and return an ancestor's slot off the receiver,
 * which is the bug the cached depth exists to prevent. */
#define BRONZE_ABI_IC_SLOTWORD_OFFSET BRONZE_ABI_IC_SLOT_OFFSET

/* ---- the METHOD-CALL site, as generated code reads it ---------------------
 *
 * A method-call site owns the same BRONZE_ABI_IC_SITE_SIZE bytes a property
 * site does — the lowerer numbers them out of one table — but its words mean
 * something different, and only the first four are used (the rest stay the
 * zero .bss gave them):
 *
 *   word 0: the receiver shape the entry was latched against, or 0 for an
 *           entry that has never latched. The guard, exactly as before.
 *   word 1: the callee's code pointer (DIRECT form; unused in slot form).
 *   word 2: low 32 bits the callee's arity; HIGH 32 bits select the form:
 *           zero is DIRECT, and any nonzero value is SLOT form carrying the
 *           receiver's own slot index PLUS ONE (so an all-zero word cannot
 *           read as slot 0).
 *   word 3: DIRECT form's env argument, passed to the code pointer verbatim.
 *           BRONZE_ABI_UNDEFINED_BITS for an env-free callee — and for every
 *           slot-form entry, so the word always parses as a Value — or the
 *           callee's environment record, a HEAP Value the module registered
 *           as a value cell via bronze_register_method_ic_cells at init.
 *
 * The DIRECT form is the original mechanism plus one load: shape match calls
 * word 1 with word 3 as env. It is latched for an env-free callee found
 * anywhere, and for an env-CARRYING callee found on the PROTOTYPE CHAIN
 * (depth >= 1), where the receiver's shape determines the holder and so the
 * function object — caching its env is exactly as sound as caching its code.
 *
 * The SLOT form is for a callee that is the receiver's OWN data property
 * (depth 0), where same-shape receivers can hold DIFFERENT functions in the
 * same slot — per-instance closures, and every host function an embedder
 * hangs on an object — so nothing about the callee may be cached. The entry
 * caches only WHERE the method lives: generated code loads the receiver's
 * slot (inline or overflow, split at BRONZE_ABI_OBJ_INLINE_SLOTS), verifies
 * the value is a Function, and calls its CURRENT code with its CURRENT env
 * and arity — the same universal dispatch bronze_dynamic_call performs, so a
 * swapped-in non-function still reaches the helper's TypeError. No heap word
 * lands in the entry at all, which is what makes the form GC-free.
 *
 * The EXOTIC form serves the receivers whose flags are NOT Plain — an Array,
 * or one of the four collections (Map/Set/WeakMap/WeakSet) — whose methods
 * are native builtins answered from an immutable C table beside the value
 * rather than from any shape-indexed slot. Word 0 then holds, instead of a
 * shape:
 *
 *      (boxOffset << BRONZE_ABI_METHOD_IC_BOX_SHIFT)
 *    | (receiver kind << BRONZE_ABI_METHOD_IC_KIND_SHIFT)
 *    | BRONZE_ABI_METHOD_IC_EXOTIC_BIT
 *
 * Bit 0 set is what distinguishes it: a real Shape* is an 8-byte-aligned
 * arena allocation and can never be odd (the same argument the property IC's
 * BRONZE_ABI_IC_SHAPE_ARRAY_METHOD sentinel makes). The kind is the
 * receiver's HeapObjectHeader::flags as latched — a runtime value the guard
 * compares against the live receiver's flags, so no kind number is baked
 * into generated code. `boxOffset` is the byte offset, from the receiver's
 * header, of the Value holding its ordinary named-property box
 * (ArrayHeader::properties / MapHeader::properties — runtime layouts, again
 * carried in the entry rather than baked into code). The guard is:
 *
 *   receiver flags == latched kind, AND the Value at boxOffset is not
 *   Object-tagged.
 *
 * That second clause is the whole shadowing story: the ONLY way one of these
 * receivers can answer a member with anything but the C table is through
 * that box — an own named property (`a.push = f`) lives in it, and a
 * subclass instance's [[Prototype]] chain hangs off it (runtime/
 * native_base.h) — so a receiver carrying one takes the helper, which walks
 * the box first exactly as the read path does. The table itself cannot
 * change: decorating `Array.prototype` is a hard error by construction
 * (rt_prop_write.cpp), and the collections have no prototype object at all —
 * their members ARE the C ladder (rt_prop.cpp), which `Object.prototype`
 * sits below, never above. Words 1–3 are the DIRECT form's: the native's
 * code pointer (a C function in the runtime image, immortal), its arity with
 * a zero high half, and BRONZE_ABI_UNDEFINED_BITS for env — a native builtin
 * is created env-free (rt_builtins.h's rtNativeFunction) and the latch
 * refuses any callee that is not. No heap word in the entry: GC-free, like
 * the slot form.
 *
 * Mixed binaries stay sound in both directions: an old hit path compares the
 * odd word against a real shape and misses; a new exotic arm compares an old
 * runtime's shape-or-zero word 0 against an odd expectation and misses. Both
 * fall to the helper, which is always correct. */
#define BRONZE_ABI_METHOD_IC_CODE_WORD   1
#define BRONZE_ABI_METHOD_IC_ARITY_WORD  2
#define BRONZE_ABI_METHOD_IC_ENV_WORD    3
#define BRONZE_ABI_METHOD_IC_SLOT_SHIFT 32
#define BRONZE_ABI_METHOD_IC_EXOTIC_BIT  1ull
#define BRONZE_ABI_METHOD_IC_KIND_SHIFT  1
#define BRONZE_ABI_METHOD_IC_BOX_SHIFT  32

/* ---- the COMPUTED-read cache, as generated code reads it -----------------
 *
 * runtime/elem_ic.h's `ElemCacheEntry`: an `InlineCache` (the same struct a
 * property site's way is, so the validity questions are literally the same
 * code) followed by the witness that pins the KEY, the arena copy of that key,
 * and the key's kind. Direct-mapped, one thread's table published into
 * `elem_cache_tbl` above.
 *
 * Generated code inlines the hit for a NUMBER or BOOLEAN key only, and the
 * omission is not an oversight: the entry's `key` is an ARENA COPY, so a live
 * key string is never the same object, and confirming a string key means a
 * length compare and a memcmp — a loop, not a guard. A string key therefore
 * keeps `bronze_elem_get`, which owns `StringHeader::equals`.
 *
 * The bucket function is splitmix64's finalizer applied twice, and generated
 * code must reproduce it EXACTLY: a probe that hashes differently from the
 * fill does not answer wrongly, it simply never hits, which is a silent
 * regression rather than a bug. elem_ic.cpp static_asserts the constants. */
#define BRONZE_ABI_ELEM_ENTRY_SIZE      48 /* sizeof(ElemCacheEntry) */
#define BRONZE_ABI_ELEM_IC_OFFSET        0 /* ElemCacheEntry::ic (InlineCache) */
#define BRONZE_ABI_ELEM_WITNESS_OFFSET  24 /* ElemCacheEntry::witness (uint64) */
#define BRONZE_ABI_ELEM_KEY_OFFSET      32 /* ElemCacheEntry::key (StringHeader*) */
#define BRONZE_ABI_ELEM_KIND_OFFSET     40 /* ElemCacheEntry::kind (uint8) */
#define BRONZE_ABI_ELEM_ENTRIES       4096 /* kElemCacheEntries, a power of two */
#define BRONZE_ABI_ELEM_KIND_NUMBER      1
#define BRONZE_ABI_ELEM_KIND_STRING      2
#define BRONZE_ABI_ELEM_KIND_BOOL        3
#define BRONZE_ABI_MIX64_ADD  0x9E3779B97F4A7C15ull
#define BRONZE_ABI_MIX64_MUL1 0xBF58476D1CE4E5B9ull
#define BRONZE_ABI_MIX64_MUL2 0x94D049BB133111EBull

/* Value: NaN-boxed, tag in the top 16 bits (runtime/value.h). */
#define BRONZE_ABI_VALUE_TAG_SHIFT      48
#define BRONZE_ABI_VALUE_PAYLOAD_MASK   0x0000FFFFFFFFFFFFull
#define BRONZE_ABI_TAG_OBJECT           0xFFF1
#define BRONZE_ABI_TAG_STRING           0xFFF2
#define BRONZE_ABI_TAG_INT32            0xFFF3
#define BRONZE_ABI_TAG_BOOL             0xFFF4
#define BRONZE_ABI_TAG_NULL             0xFFF5
#define BRONZE_ABI_TAG_UNDEFINED        0xFFF6
#define BRONZE_ABI_TAG_HOLE             0xFFF7
/* Above the number range like every other tag, and named here because the
 * inline `===` needs to tell the two values whose equality is NOT bit
 * equality — a string and a BigInt — from every value whose equality is.
 * runtime/value.h holds the definition; runtime/rt_convert.cpp asserts the
 * two agree. */
#define BRONZE_ABI_TAG_BIGINT           0xFFFB
#define BRONZE_ABI_CANONICAL_NAN_BITS   0x7FF8000000000000ull
#define BRONZE_ABI_NUMBER_MAX_BITS      0xFFF0000000000000ull
#define BRONZE_ABI_HOLE_BITS            0xFFF7000000000000ull

/* HeapObjectHeader::flags, and the values that mean "a plain object" as
 * opposed to an array (1), a function (2), a typed-array view (3) or an
 * ArrayBuffer (4). All of them reach bronze_prop_get, so the fast path has
 * to discriminate on this before it believes anything else. */
#define BRONZE_ABI_OBJ_FLAGS_OFFSET      2
#define BRONZE_ABI_OBJ_FLAGS_PLAIN       0
#define BRONZE_ABI_OBJ_FLAGS_ARRAY       1
#define BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY 3

/* ArrayHeader field offsets */
#define BRONZE_ABI_ARRAY_LENGTH_OFFSET   8
#define BRONZE_ABI_ARRAY_CAPACITY_OFFSET 12
#define BRONZE_ABI_ARRAY_HEAD_OFFSET     16
#define BRONZE_ABI_ARRAY_ELEMS_OFFSET    24
#define BRONZE_ABI_ARRAY_PROPS_OFFSET    32

/* Environment records (runtime/env.h EnvHeader): the parent link, then the
 * slot array. Generated code inlines captured-variable reads and writes —
 * a load per depth step and a load or store of the slot — with every guard
 * failure (wrong tag, wrong kind, short chain, slot out of range) routed to
 * the helper, which still owns the fatal that names the lowering bug.
 * Pinned by static_asserts in runtime/env.h. */
#define BRONZE_ABI_OBJ_FLAGS_ENV        12
#define BRONZE_ABI_ENV_PARENT_OFFSET     8
#define BRONZE_ABI_ENV_SLOTS_OFFSET     16

/* HeapObjectHeader::size — total object bytes, header included. The inline
 * environment path reads it to keep the helper's slot-range tripwire: a slot
 * index past the record is a lowering bug and must still reach the fatal
 * rather than a load past the object. Pinned in runtime/object.h. */
#define BRONZE_ABI_HDR_SIZE_OFFSET       4

/* Total bytes of HeapObjectHeader itself — where a heap block's payload
 * starts. The method-IC slot form reads an overflow block's Value array
 * through it. Pinned in runtime/object.h. */
#define BRONZE_ABI_HDR_BYTES             8

/* TypedArrayHeader (runtime/typed_array.h): the buffer Value, the window,
 * and the element kind. BUF_EXTPTR_OFFSET is the buffer's external-storage
 * word: zero for an ordinary buffer (bytes inline, starting at
 * BUF_DATA_OFFSET == sizeof(ArrayBufferHeader)), or the address of a
 * NON-MOVING host byte store (embed.h externalizeArrayBuffer) — the inline
 * element paths select between the two, which is the whole cost of shareable
 * buffers. The two float kinds are the ones the dynamic-index element fast
 * path inlines; every other kind keeps the helper's conversion ladder.
 * Pinned in runtime/typed_array.h. */
#define BRONZE_ABI_TA_BUFFER_OFFSET      8
#define BRONZE_ABI_TA_BYTEOFFSET_OFFSET 16
#define BRONZE_ABI_TA_LENGTH_OFFSET     20
#define BRONZE_ABI_TA_KIND_OFFSET       24
#define BRONZE_ABI_TA_KIND_INT8          0
#define BRONZE_ABI_TA_KIND_UINT8         1
#define BRONZE_ABI_TA_KIND_UINT8CLAMPED  2
#define BRONZE_ABI_TA_KIND_INT16         3
#define BRONZE_ABI_TA_KIND_UINT16        4
#define BRONZE_ABI_TA_KIND_INT32         5
#define BRONZE_ABI_TA_KIND_UINT32        6
#define BRONZE_ABI_TA_KIND_FLOAT32       7
#define BRONZE_ABI_TA_KIND_FLOAT64       8
/* The first kind whose elements are BIGINTS rather than Numbers. Generated
 * code never stores to these inline, but the dynamic-store fast path must
 * KNOW where they start: an out-of-bounds store on a Number kind is a
 * discard (10.4.5.16 — ToNumber of a number is the number), while on a
 * BigInt kind the same store still owes the ToBigInt that throws for a
 * Number value, so it must reach the helper. kind < BIGINT64 is that test. */
#define BRONZE_ABI_TA_KIND_BIGINT64     10
#define BRONZE_ABI_BUF_EXTPTR_OFFSET    24
#define BRONZE_ABI_BUF_DATA_OFFSET      32

/* ObjectHeader: the shape word, then the out-of-line overflow Value, then
 * kInlineSlots inline Values. */
#define BRONZE_ABI_OBJ_SHAPE_OFFSET      8
#define BRONZE_ABI_OBJ_OVERFLOW_OFFSET  16
#define BRONZE_ABI_OBJ_SLOTS_OFFSET     24
#define BRONZE_ABI_OBJ_INLINE_SLOTS      4

/* Shape (runtime/shape.h): the fields the two inline IC fast paths read, all
 * deliberately laid out BEFORE the transitions vector so no standard-library
 * type's size can shift them between build configurations. Shapes are immortal
 * and non-moving, so generated code may chase these pointers freely.
 *
 * The depth > 0 READ walk needs root -> prototype (the chain step) and dict
 * (a dictionary on the path is the miss `cachedProtoHolder` answers). The
 * shape-transition WRITE hit needs parent (is the cached shape one add above
 * the receiver's?), slot_index (does the cached shape's own node carry the
 * site's key? — slot uniqueness along a chain makes `slot_index ==
 * cached_slot` that exact question), the four attribute bytes as one word
 * (an assignment creates enumerable/writable/configurable data properties
 * and nothing else may be cached into one), and used_as_prototype (a marked
 * receiver must bump the epoch, which is the helper's job). Pinned by
 * static_asserts in runtime/shape.h. */
#define BRONZE_ABI_SHAPE_PARENT_OFFSET      0
#define BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET  16
#define BRONZE_ABI_SHAPE_ATTRS_OFFSET      20
/* enumerable=1, accessor=0, writable=1, configurable=1 as the one
 * little-endian u32 the four adjacent bool bytes spell. */
#define BRONZE_ABI_SHAPE_ATTRS_PLAIN_DATA  0x01010001u
#define BRONZE_ABI_SHAPE_ROOT_OFFSET       24
#define BRONZE_ABI_SHAPE_PROTO_OFFSET      32
#define BRONZE_ABI_SHAPE_DICT_OFFSET       40
#define BRONZE_ABI_SHAPE_USEDPROTO_OFFSET  48
/* Shape::family_stamp — the layout-family id `bronze_family_stamp` writes and
 * a family guard reads. It lives on the SHAPE and not on the object because
 * that is where the fact belongs: every object at a shape has that shape's
 * property names at that shape's slots, so one verification answers for all of
 * them, and an object header stays the width it was. The guard already loads
 * the shape pointer for the identity form, so this is one more load off memory
 * that is immortal, shared by every instance, and hot. */
#define BRONZE_ABI_SHAPE_FAMILY_OFFSET     56
/* Not a class id: zero is "never looked at" and one is "looked at and matched
 * no registered class". Both fail every range compare, because the runtime
 * hands out ids from BRONZE_ABI_FAMILY_FIRST_ID up. */
#define BRONZE_ABI_FAMILY_UNSTAMPED        0
#define BRONZE_ABI_FAMILY_NONE             1
#define BRONZE_ABI_FAMILY_FIRST_ID         2

/* FunctionHeader::code — the identity the Math direct-dispatch guard compares
 * (see the bronze_math_* registry entries). Pinned in runtime/fn.cpp. */
#define BRONZE_ABI_OBJ_FLAGS_FUNCTION    2
#define BRONZE_ABI_FN_CODE_OFFSET        8

/* The FunctionHeader fields the inline `new` fast path reads, and the vet
 * byte that gates it. `construct_vetted` is set by exactly one line in the
 * runtime — bronze_construct's ordinary path, after the bound-function and
 * primitive-wrapper probes have both missed and the prototype/instance_shape
 * pair exists — so a set byte means "the helper has already taken the plain
 * path for this function object", which is monotone: a function's code
 * pointer never changes, so it can never later become bound or a wrapper
 * constructor. Reassigning `.prototype` swaps `instance_shape` in the same
 * write (rt_prop_write.cpp) and never nulls it, so the vetted fast path
 * reads whatever is current. Pinned in runtime/fn.h. */
#define BRONZE_ABI_FN_ENV_OFFSET            16
#define BRONZE_ABI_FN_PROTOTYPE_OFFSET      24
#define BRONZE_ABI_FN_PROPERTIES_OFFSET     32
#define BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET 40
#define BRONZE_ABI_FN_ARITY_OFFSET          56
#define BRONZE_ABI_FN_CTOR_VETTED_OFFSET    65

/* Total bytes of a plain object with no internal slots — header, shape word,
 * overflow word, and the kInlineSlots inline Values — which is the one size
 * the inline `new` fast path allocates. Already 8-aligned, so it is also the
 * exact amount the bump cursor advances. Pinned in runtime/object.cpp. */
#define BRONZE_ABI_PLAIN_OBJECT_BYTES    56

/* One entry of a module's fn-singleton table: the code pointer, then the Value.
 * An entry answers a mention only when its code word matches the mention's own
 * function pointer, so a slot that was never filled (a zeroed table) misses,
 * and the by-code-pointer map in rt_state.cpp stays the authority on identity
 * across every module in the process. */
#define BRONZE_ABI_FNSLOT_SIZE          16
#define BRONZE_ABI_FNSLOT_CODE_OFFSET    0
#define BRONZE_ABI_FNSLOT_VALUE_OFFSET   8

/* Every Object-tagged heap allocation has at least this many payload bytes,
 * which is what makes the fast path's unconditional load of the shape word
 * (offset 8..15) safe BEFORE the flags discrimination has passed. Pinned by
 * static_asserts over every Object-tagged header in rt_helpers.cpp. */
#define BRONZE_ABI_OBJ_MIN_PAYLOAD       8

/*
 * A generated function's GC root frame: allocated in the
 * function's own stack frame, linked onto its thread's `frame_top` (in the
 * bronze_tls_block below) on entry and unlinked before every return, so the
 * collector can find every Dynamic value compiled code is holding. Generated
 * code links and unlinks inline — no helper call — because the call-heavy
 * path pays this per invocation.
 *
 * `slots` is `count` entries long, inline after the header.
 */
typedef struct bronze_gc_frame {
    struct bronze_gc_frame* prev;
    uint64_t count;
    uint64_t slots[1];
} bronze_gc_frame;

/*
 * The per-thread ABI data block: every mutable word generated code shares
 * with the runtime, one instance per OS thread, fetched once per compiled
 * function through bronze_tls_block_addr() and read or written at fixed byte
 * offsets from that base. One block rather than one thread_local per word
 * because Windows cannot import a thread_local across a DLL boundary, so
 * per-thread data costs a call — and one call covering all eleven words is
 * the cheapest that call gets.
 *
 * The layout is ABI: the BRONZE_TLS_*_OFF constants below are what codegen
 * emits, the runtime static_asserts them against this struct (tls_block.cpp),
 * and any change here moves the fingerprint. Field order is by heat — the
 * frame link and the exception cell are touched per call.
 *
 * The fields:
 *
 *  - frame_top: head of this thread's chain of bronze_gc_frame records.
 *
 *  - exception_cell: the pending exception — the thrown value, or
 *    BRONZE_ABI_NO_EXCEPTION_BITS when nothing is pending. Generated code,
 *    after every instruction that can throw, loads it, compares it against
 *    that constant and branches — no helper call, for the same reason the GC
 *    frame is linked inline: this is on the call-heavy path. Two rules the
 *    runtime side must keep, because nothing enforces them: a helper that
 *    sets the cell RETURNS BRONZE_ABI_UNDEFINED_BITS (its caller stores the
 *    result into a GC root slot before it tests the cell, and the collector
 *    reads every slot of a linked frame); and the cell is a permanent GC
 *    root — a thrown object is live for exactly as long as it is pending,
 *    across an arbitrary number of frames, and nothing else roots it. There
 *    is no unwind ABI beyond this: propagation is an ordinary `ret`, so the
 *    frame pop before it is the one the non-throwing path already emits.
 *
 *  - proto_epoch: the prototype-mutation epoch (runtime/object.h): what
 *    makes a cached depth > 0 property hit sound, read inline by the
 *    proto-hit fast path.
 *
 *  - alloc_cursor / alloc_limit: the inline-allocation window.
 *    [cursor, limit) is heap memory the runtime has carved out of from-space
 *    for generated code to bump-allocate plain `new` instances from
 *    (heap.cpp owns both). The inline path only ever ADVANCES cursor when
 *    the object fits — it can never collect — and every miss (window empty,
 *    invalidated, or disabled) falls back to bronze_construct, which refills
 *    it. Both words are zeroed by every collection, because the window
 *    points into the semispace the collector is abandoning. Zero/zero is
 *    also the initial and the BRONZE_NO_INLINE_ALLOC=1 state: the unsigned
 *    subtraction limit-cursor is then 0, no size fits, and the fast path is
 *    dormant.
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
#define BRONZE_ABI_MU64   uint64_t*
#define BRONZE_ABI_TLSPTR bronze_tls_block*
#define BRONZE_ABI_FNPTR  bronze_fn_code
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
#undef BRONZE_ABI_MU64
#undef BRONZE_ABI_TLSPTR
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS

#ifdef __cplusplus
}
#endif

#endif /* BRONZE_ABI_H */
