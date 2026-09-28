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
    /* The same block, fetched by a module's entry function into the pinned\
     * register (bronze_abi_tls.h), and the first fetch on a thread arms\
     * `stack_limit`. `bronze_stack_overflow` is what a prologue calls when\
     * its stack pointer is below that limit: it raises the RangeError. */ \
    X(bronze_tls_enter,           BRONZE_ABI_VPTR, (BRONZE_ABI_NOARGS)) \
    X(bronze_stack_overflow,      BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
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
    X(bronze_unbox_str,           BRONZE_ABI_CSTR, (BRONZE_ABI_U64)) \
    X(bronze_create_object,       BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    /* `coro.start`: a coroutine body's fresh brass frame (a raw address)\
     * and its BRONZE_ABI_CORO_* kind; answers the generator object or the\
     * async function's promise. */ \
    X(bronze_coro_start,          BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_async_iter_open,     BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_iter_next,     BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_async_iter_close,    BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_dynamic_import,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_module_namespace,    BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_object_keys,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_for_in_keys,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_method_def,          BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_method_def_computed, BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_accessor_def,        BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_accessor_def_computed, BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    /* ONE key of an `Object.defineProperties(o, { k: {...}, ... })` whose
     * descriptors are object literals: 10.1.6.3 [[DefineOwnProperty]] driven
     * from a BRONZE_ABI_DESC_* mask instead of from a descriptor object.
     *
     * It exists because no other helper can say what the mask says. The two
     * definition helpers above fix their attributes (a method is writable and
     * configurable, an accessor's `enumerable` is their only choice) and
     * neither runs 10.1.6.3's validation, so neither can refuse a
     * redefinition; `bronze_prop_set` is an assignment and cannot name an
     * attribute at all. Handing this the mask is what lets the six descriptor
     * objects three.js builds per `Object3D` never be built — the literal
     * already decided every field, and 6.2.6.5's job was to find that out.
     *
     * `target` is checked exactly as `Object.defineProperties` checks it, and
     * with the same member name in the message, so a non-object throws the
     * same TypeError here as there. Refusals are 20.1.2.4's: a TypeError, not
     * a boolean. */ \
    X(bronze_define_own_attr,     BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U32)) \
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
    /* The SLOW HALF of a cached provided-global read. Generated code reads a
     * provided global through a per-module cache cell — one u64 per distinct
     * name the module mentions, in the module's own .data, holding
     * BRONZE_ABI_HOLE_BITS until filled — and reaches this
     * only when the cell is the hole: (key id, the module's cell array, its
     * cell count, this name's slot). The helper registers the array as a root
     * span the first time it sees it (so the collector forwards the cached
     * Values in place, and so a host re-registering a global can put the hole
     * back in every module's cell for it), resolves the name the way
     * bronze_global_get does, and fills the slot for a builtin OR a
     * host-registered answer — the host registry is what the cache exists to
     * take off the per-read path. A `globalThis.x` the program assigned is
     * still answered but never cached, because the program can assign again. */ \
    X(bronze_global_get_cached,   BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_MU64, BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_resolve_name,        BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_BOOL)) \
    X(bronze_immutable_assign,    BRONZE_ABI_U64,  (BRONZE_ABI_NOARGS)) \
    /* A `--pins` claim CONTRADICTED by a value the program actually produced
     * (src/types/pins.h). The u32 is a registered key index holding the
     * manifest line as the manifest spells it, and the u64 is the offending
     * value, which the message names by type. It raises a TypeError and never
     * returns; generated code follows the call with `unreachable`. Emitted
     * only on the COLD arm of a barrier, so a program that keeps its promises
     * never reaches it. */ \
    X(bronze_pin_violation,       BRONZE_ABI_U64,  (BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    /* The same barrier for the one pin whose shape has no inline test: a
     * `numeric-elements` FIELD must hold a plain, dense JS Array, which is an
     * object tag plus a header read plus a class comparison. Check and raise
     * are one helper because the site is a constructor-time store, never a
     * loop-carried one. */ \
    X(bronze_pin_check_array,     BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_U64)) \
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
    X(bronze_env_ancestor,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    /* The environment ACCESS-GUARD tripwire: what the inline path branches to\
     * when the object tag, the Env brand or the slot range says the resolved\
     * (depth, index) does not describe the record it was handed. Every one of\
     * those is a lowering bug rather than anything a program can do, so this\
     * does not RETURN — it re-derives the same three questions to say which\
     * one failed and fatals. Declared noreturn on the LLVM side too, which is\
     * the point: the failure edge ends in `unreachable`, so the guard costs a\
     * compare and a never-taken branch and puts no merge, no phi and no\
     * clobbering call into the flow the fast path is optimized in. */ \
    X(bronze_env_access_failed,   BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32)) \
    X(bronze_prop_get,            BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_MU64)) \
    X(bronze_get_prototype_of,    BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_super_get,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
    X(bronze_super_elem_get,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    /* The trailing BOOL is the strictness of the code the write was written in,\
     * on the same rule bronze_prop_set carries one: `super.k = v` is an ordinary\
     * Reference, so 13.15.2 raises for a refused Set in strict code and discards\
     * in sloppy code, and only the compiler still knows which this site was. */ \
    X(bronze_super_set,           BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_super_elem_set,      BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_BOOL)) \
    X(bronze_prop_set,            BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_MU64, BRONZE_ABI_BOOL)) \
    /* The out-of-line halves of the inline property paths, for code built\
     * to be small (the JIT's lower tiers). `_poly_scan` is a read's scan of\
     * its site's ways 1..N-1 after a way-0 miss: the address of the slot to\
     * load when a way describes the plain receiver's own data property, else\
     * 0; it reads, never allocates. The `_counted` forms are bronze_prop_get\
     * and _set. Each first increments the speculation counter it is handed\
     * (null: none), the count the inline slow path would have kept. */ \
    X(bronze_prop_poly_scan,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_MU64, BRONZE_ABI_MU32)) \
    X(bronze_prop_get_counted,    BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_MU64, BRONZE_ABI_MU32)) \
    X(bronze_prop_set_counted,    BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_MU64, BRONZE_ABI_BOOL, BRONZE_ABI_MU32)) \
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
    X(bronze_array_pop,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_array_push,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_array_shift,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
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
    /* A left-associative `+` spine of three or more operands, as an\
     * ACCUMULATOR rather than as N-1 calls to bronze_dynamic_add. Each of the\
     * three is exactly 13.15.3 over the pair in front of it, so the chain\
     * evaluates in the order the clause states: `begin` is the first `+`,\
     * every `append` is the next one, and `end` converts nothing at all.\
     *\
     * What changes is the intermediate. `((a + b) + c) + d` over strings\
     * allocates three flat results today and copies the whole prefix into\
     * each one, which is quadratic in the number of operands; here `begin`\
     * allocates ONE string with room to grow and each `append` writes its\
     * piece into the slack. The last argument of `begin` is how many operands\
     * are still to come, which is a compile-time fact and only a sizing hint\
     * — a wrong one costs a reallocation, never an answer.\
     *\
     * The accumulator is an ordinary Tag::String heap value whose `length` is\
     * the text written so far and whose allocation reserves more, marked in\
     * the HEAP header's `flags` word whose every other bit a String leaves\
     * zero. Two things follow, and both are the reason the form is this one\
     * rather than a builder object. It is scanned, moved and read like any\
     * other string, so generated code roots it the way it roots any Dynamic\
     * value and no collector learns a new shape. And it is a CORRECT string\
     * at every point in the chain, so an operand that throws half way through\
     * leaves a value that is merely garbage rather than one that would be a\
     * type confusion if anything found it.\
     *\
     * `append` mutates in place only what `begin` or a previous `append`\
     * minted and handed it exactly once — the lowerer emits the spine so that\
     * each accumulator has a single use, and the IL verifier rejects a shape\
     * where it does not. The mark is the second guard rather than the first:\
     * an `append` handed anything without it copies, so no rule about who\
     * points at a string can be violated by mutating one. `end` clears the\
     * mark and is the identity on the text, which is also what makes a chain\
     * that turned out NUMERIC free — nothing was ever marked, and every step\
     * was the addition it would have been anyway. */ \
    X(bronze_concat_begin,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_concat_append,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_concat_end,          BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
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
    /* The calling thread's INSTANCE of the module's writable tables, the
     * entry's first call (before any registration above, which it feeds).
     * A module's writable tables — environment cell, template cells, global
     * cache, inline-cache table, native import table — are one contiguous
     * run of its data, [`__bronze_instance`, `__bronze_instance_end`), and
     * every address generated code forms into them is the image address
     * plus the calling thread's DELTA. The first thread to enter the module
     * (its home) uses the image's run itself, delta 0; every later thread
     * gets a copy of the run as it was before the home thread wrote to it.
     * `slotCell` is the module's `__bronze_module_slot`, a u64 OUTSIDE the
     * run that the first call fills with a process-wide slot number; the
     * answer is also stored at that slot of the thread's
     * `bronze_tls_block.module_deltas`, which is where every function other
     * than the entry reads it. Idempotent per thread. So one image runs on
     * any number of threads, each against its own heap; the one rule left is
     * that a thread runs the module's entry before its code. */ \
    X(bronze_module_instance,      BRONZE_ABI_U64,  (BRONZE_ABI_MU64, BRONZE_ABI_MU64, BRONZE_ABI_MU64)) \
    /* ---- host natives (runtime/native_registry.cpp) ------------------------
     *
     * A native is a C function the HOST registered under a JS path
     * (embed::registerNative) and the compiler lowered a call site to — a
     * direct machine call with unboxed arguments, no dynamic dispatch. The
     * module never names the function's symbol: it calls through a slot of
     * its own import table (`<entry>_native_imports`, the loadable-module
     * section below), which the runtime fills BY NAME from the registry at
     * load time. Nothing about a native is resolved by a linker.
     *
     * `bronze_native_bind` fills the calling module's table from the current
     * thread's registry, and is FATAL — naming every slot it could not fill —
     * when a native the module was compiled against is unregistered or was
     * registered with a different signature. The module's entry calls it
     * first thing, so a host that never asked (embed::bindNativeImports is
     * the soft form, for a loader that would rather refuse by name) still
     * gets a named refusal rather than a jump through zero. Idempotent, so a
     * loader that bound first costs the entry one no-op rebind.
     *
     * `bronze_native_unbound` is the trap a never-bound slot jumps to. */ \
    X(bronze_native_bind,         BRONZE_ABI_VOID, (BRONZE_ABI_MU64)) \
    X(bronze_native_unbound,      BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    /* The receiver or a class-typed argument of a native: the raw data
     * pointer of a handle whose class tag is `classInfo`, or a TypeError
     * naming the class (and what arrived instead) with a null return. One
     * compare — the tag is a word in the handle cell. `classInfo` arrives
     * from a `class <path>` import slot. */ \
    X(bronze_native_handle_data,  BRONZE_ABI_VPTR, (BRONZE_ABI_U64, BRONZE_ABI_CVPTR)) \
    /* The `void*` a native constructor or class-returning native produced,
     * as a handle of the class: born on the class's prototype, tagged, owing
     * the class's destructor. A null pointer becomes `null`. ALLOCATES. */ \
    X(bronze_native_wrap,         BRONZE_ABI_U64,  (BRONZE_ABI_VPTR, BRONZE_ABI_CVPTR)) \
    /* A typed-array argument's element 0, for a native declared to take
     * `<kind>[]` — the u32 is the ElementKind the declaration names. Not a
     * typed array, another element kind, or a detached buffer: a TypeError
     * and null. The pointer is valid until the next allocation, so the
     * lowering takes it AFTER every argument's scalar coercion (ToInt32 can
     * run user code) and immediately before the call, and the native must
     * not allocate through the embed API while it holds it. */ \
    X(bronze_native_typed_array_data,   BRONZE_ABI_VPTR, (BRONZE_ABI_U64, BRONZE_ABI_U32)) \
    X(bronze_native_typed_array_length, BRONZE_ABI_U32,  (BRONZE_ABI_U64)) \
    /* A typed-array RETURN, for a native declared to answer `<kind>[]`: a C
     * function returning void with one extra trailing `bronze_native_buffer*`
     * parameter (abi/bronze_native_type.h). The thunk calls
     * `bronze_native_buffer_slot` for a zeroed per-thread descriptor (a
     * stack, so a native that re-enters the program and reaches another
     * buffer-returning native never shares a slot), passes its address as
     * the trailing argument, calls the native, then calls
     * `bronze_native_buffer_wrap` with the ElementKind the declaration names;
     * wrap pops the slot and answers the Value: a fresh JS-owned typed array
     * holding a copy when `release` is null, a view over the native's own
     * bytes owing `release(ctx)` at collection when it is not, the empty
     * array for a null `data`. The native is called through an invoke; when
     * it throws, the thunk's landing pad calls `bronze_native_buffer_abandon`,
     * which pops the slot and calls the release of a block the native had
     * transferred, and raises the value again. ALLOCATES. */ \
    X(bronze_native_buffer_slot,    BRONZE_ABI_VPTR, (BRONZE_ABI_NOARGS)) \
    X(bronze_native_buffer_wrap,    BRONZE_ABI_U64,  (BRONZE_ABI_U32)) \
    X(bronze_native_buffer_abandon, BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    /* A `str` argument as the NUL-terminated UTF-8 a C native takes: the
     * value (ToString for a non-string; undefined, a missing argument, is
     * "") copied into a per-thread scratch stack, whose top `count` entries
     * `bronze_native_str_release` pops after the call. Nested native calls
     * push and pop LIFO, so an outer native's text survives an inner call.
     * ALLOCATES for a non-string, so the lowering converts every `str`
     * before it takes any typed-array pointer. `bronze_native_str_from_utf8`
     * is the return direction: the `const char*` a native answered, as a
     * string value (null → ""). ALLOCATES. */ \
    X(bronze_native_str_utf8,      BRONZE_ABI_CVPTR, (BRONZE_ABI_U64)) \
    X(bronze_native_str_release,   BRONZE_ABI_VOID,  (BRONZE_ABI_U32)) \
    X(bronze_native_str_from_utf8, BRONZE_ABI_U64,   (BRONZE_ABI_CVPTR)) \
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
    /* THE SLOT-REPRESENTATION ELIGIBILITY LIST (stage R1,
     * src/runtime/slot_repr.h). `fields` is `count` of the module's own key
     * indices — the property NAMES a `--pins` manifest declared `number` on
     * some class whose layout this compilation proved — turned into
     * process-wide ids through `keyMap`, exactly as the family table's names
     * are, and for the same reason: this call follows the
     * `bronze_register_key_string` loop.
     *
     * It is an ELIGIBILITY list and not a layout claim. What it licenses is
     * narrow: a shape transition that FIRST installs one of these names, with
     * a Number in hand, may give that slot the double representation. Nothing
     * here says a slot IS a double — the shape says that, and the runtime's
     * generalization takes it back the moment a store contradicts it. So a
     * name that is pinned on one class and dynamic on another costs at most a
     * shape split, never a wrong read.
     *
     * By NAME rather than by (class, slot) deliberately. The runtime meets a
     * transition, not a class: `bronze_family_stamp` recognises a class only
     * after its shape already exists, which is far too late to decide how the
     * slot is stored. A name list is the fact that is available at the one
     * moment the decision has to be made. Registration-only. */ \
    X(bronze_register_slot_repr,  BRONZE_ABI_VOID, \
      (BRONZE_ABI_PU32, BRONZE_ABI_U32, BRONZE_ABI_PU32)) \
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
    /* THE PIN CENSUS (`bronze build --census`, src/runtime/pin_census.h). A
     * census build hands over, at module init, the manifest path to write and
     * the whole SITE TABLE — `count` pairs of (module key index, site info),
     * turned into process-wide key ids through `keyMap` exactly as the class
     * family table is. Registration is separate from recording because a site
     * the run never reaches is still a fact: "never observed" and "not a site"
     * are different answers, and a STATIC refusal has to disqualify its entry
     * on a run that never touches it.
     *
     * The record call is one observation: the value that reached the site. It
     * is emitted at exactly the places lowering has no static answer left, and
     * a census build is never benchmarked, so it is a plain call with no fast
     * path and no inline form. */ \
    X(bronze_census_register,     BRONZE_ABI_VOID, (BRONZE_ABI_CSTR, BRONZE_ABI_PU32, BRONZE_ABI_U32, BRONZE_ABI_PU32)) \
    X(bronze_census_record,       BRONZE_ABI_VOID, (BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_U64)) \
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
    X(bronze_math_imul,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_floor,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_ceil,           BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_round,          BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_string_char_code_at, BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_math_sin_f64,        BRONZE_ABI_F64,  (BRONZE_ABI_F64)) \
    X(bronze_math_cos_f64,        BRONZE_ABI_F64,  (BRONZE_ABI_F64)) \
    X(bronze_math_min2_f64,       BRONZE_ABI_F64,  (BRONZE_ABI_F64, BRONZE_ABI_F64)) \
    X(bronze_math_max2_f64,       BRONZE_ABI_F64,  (BRONZE_ABI_F64, BRONZE_ABI_F64)) \
    /* Method call IC runtime helpers: property lookup + IC update + dispatch. */ \
    X(bronze_call_method,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U32, BRONZE_ABI_PU64, BRONZE_ABI_MU64)) \
    X(bronze_call_method_spread,  BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_U64, BRONZE_ABI_MU64)) \
    /* Brass IL translator bridge and runtime helper exports. */ \
    X(bronze_f64_mod,             BRONZE_ABI_F64,  (BRONZE_ABI_F64, BRONZE_ABI_F64)) \
    X(bronze_create_func,         BRONZE_ABI_U64,  (BRONZE_ABI_FNPTR, BRONZE_ABI_I32, BRONZE_ABI_U64)) \
    X(bronze_global_get_name,     BRONZE_ABI_U64,  (BRONZE_ABI_CSTR)) \
    X(bronze_call_dynamic_0,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_1,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_2,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_3,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_4,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_5,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_6,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_7,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_8,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_9,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_10,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_11,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_12,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_13,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_14,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_15,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_16,     BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_call_dynamic_n,      BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_super_call_0,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_1,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_2,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_3,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_4,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_5,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_6,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_7,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_8,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_9,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_10,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_11,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_12,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_13,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_14,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_15,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_16,       BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_super_call_n,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_construct_0,         BRONZE_ABI_U64,  (BRONZE_ABI_U64)) \
    X(bronze_construct_1,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_2,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_3,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_4,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_5,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_6,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_7,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_8,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_9,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_10,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_11,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_12,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_13,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_14,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_15,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_16,        BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_construct_n,         BRONZE_ABI_U64,  (BRONZE_ABI_U64, BRONZE_ABI_U32, BRONZE_ABI_PU64)) \
    X(bronze_register_key_manifest, BRONZE_ABI_VOID, (BRONZE_ABI_PU8, BRONZE_ABI_MU32)) \
    X(bronze_print_f64,           BRONZE_ABI_VOID, (BRONZE_ABI_F64)) \
    X(bronze_print_i32,           BRONZE_ABI_VOID, (BRONZE_ABI_I32)) \
    X(bronze_print_dynamic,       BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_space,         BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    X(bronze_print_newline,       BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    X(bronze_print_f64_err,       BRONZE_ABI_VOID, (BRONZE_ABI_F64)) \
    X(bronze_print_i32_err,       BRONZE_ABI_VOID, (BRONZE_ABI_I32)) \
    X(bronze_print_dynamic_err,   BRONZE_ABI_VOID, (BRONZE_ABI_U64)) \
    X(bronze_print_space_err,     BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    X(bronze_print_newline_err,   BRONZE_ABI_VOID, (BRONZE_ABI_NOARGS)) \
    X(brass_gc_write_barrier,     BRONZE_ABI_VOID, (BRONZE_ABI_U64, BRONZE_ABI_U64)) \
    X(bronze_register_code_ranges,   BRONZE_ABI_VOID, (BRONZE_ABI_CVPTR, BRONZE_ABI_U32)) \
    X(bronze_unregister_code_ranges, BRONZE_ABI_VOID, (BRONZE_ABI_CVPTR, BRONZE_ABI_U32))

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
 * The brass runtime symbols outside the registry a compiled module names,
 * which the shared runtime exports beside it (cmake/bronze_abi_exports.cmake
 * reads these lines; src/cli/link.cpp offers them as imports):
 *
 * - The raise entry points a MIR `throw` / `rethrow` outside any `try` calls.
 * - The personality routine the module's unwind information points at, so a
 *   throw lands at its landing pads: named from .xdata on Windows and from
 *   the .eh_frame CIE on ELF and Mach-O. The runtime defines the one for its
 *   platform.
 *
 * brass defines each as brass_default_<rest>; the runtime exports the
 * canonical name as an alias of it, the one definition of that name in the
 * process.
 */
#define BRONZE_ABI_BRASS_SYMBOLS(Y) \
    Y(brass_throw) \
    Y(brass_rethrow)

/*
 * The brass runtime functions a compiled module names under their own names,
 * which the shared runtime exports as they are (brass defines them; nothing
 * overrides them):
 *
 * - brass_coro_create: a generator or async function's stub creating its
 *   body's coroutine frame, in code compiled outside a tiered program.
 */
#define BRONZE_ABI_BRASS_EXPORTS(Z) \
    Z(brass_coro_create)

#define BRONZE_ABI_PERSONALITY_WINDOWS brass_seh_personality
#define BRONZE_ABI_PERSONALITY_SYSV    brass_sysv_personality

