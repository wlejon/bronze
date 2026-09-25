# il2mir: bronze IL to brass MIR

## 1. Overview
`src/codegen-brass/il2mir/` lowers bronze's IL to brass MIR. It lives in bronze, and brass knows nothing about it. The pipeline is `bronze::il::Module` → `il_to_brass_ast` (`codegen-brass/il_to_brass_ast.*`) → `il2mir::BronzeModuleAST` → `il2mir::translate_bronze_ast` → `brass::Module`. `BrassBackend` then runs that module through brass's pass pipeline (`il2mir::pass_pipeline_options`) for AOT objects, the JIT, or the tiered engine.

Everything is in the top-level namespace `il2mir`, which pulls in `brass` (`il_ast.h`). The value/TLS/IC layout constants come from the bronze ABI header (`il_abi.h`), not from copies.

The lowered code calls the bronze runtime (`bronze_*`, `brass_gc_*`) by name. Brass resolves those names through its generic host-symbol hook, `brass/runtime/host_symbols.hpp`. `codegen-brass/brass_symbol_registration.cpp` installs a `HostSymbolProvider` that binds the real bronze ABI (the `BRONZE_ABI_FUNCTIONS` X-macro), and the math and parallel helpers into every engine a brass tier creates: the fast interpreter, the baseline JIT, the tier-2 installer JIT, deopt and the reference interpreter.

Bronze's objects live on brass's collector. Each thread's `bronze::Heap` (`runtime/heap.h`) owns one `brass::gc::Heap` and binds it as the thread's current heap, so every object bronze-run code creates (objects, arrays, environments, closures, strings, and the `.resume` closures that carry a generator's or async function's state) is a brass heap object whose payload begins with bronze's `HeapObjectHeader`. Bronze registers the object layouts and trace functions (`runtime/heap_trace.cpp`), and the brass interpreters on the thread register their frames with that same heap. The inline allocation paths (`il_alloc_lowering.cpp`) bump the thread's TLS allocation window, which is bound as the brass heap's allocation buffer, and write brass's cell header (`gc_cell_header` in the TLS block) in front of each bronze header. Bronze's `brass_gc_write_barrier` (`runtime/gc.cpp`) is the process's only definition of that name: it strips the NaN-box tag from the object operand and takes brass's card barrier on whichever of the thread's heaps holds the object, overriding brass's `brass_default_gc_write_barrier` through the symbol table.

Compiled code roots its JS values through brass stack maps, not through anything bronze keeps. A `Dynamic` value crosses every compiled-function boundary as MIR `tagged` (`lower_abi_type`): SSA results, block params, and the params and returns of functions with a body. Native imports keep raw `i64` signatures. Arithmetic, tag tests and runtime calls work on `i64`, so every use unboxes with `bitcast.i64.tagged` (`get_val_by_id`) and every result boxes back with `bitcast.tagged.i64` (`set_inst_result`). `unbox_tagged` folds a box that sits in the same block with no call or safepoint between it and the use, and `ensure_type` folds `box(unbox(x))`, so straight-line code keeps its values in registers. Brass records every tagged value live at a safepoint, in a register, a spill slot or an interpreter register, as a stack-map root whose `value` kind is `Tagged`. The collector updates it in place and keeps its NaN-box tag. Arguments passed through memory (a construct, a method call, a super call, or a dynamic call with more than 16 arguments) go through one `alloca.tagged` argv block per function, sized to the widest such call. The block is zeroed, and each of its words is a root at every safepoint. A value is a root only while it is live. A native call hands the native raw pointers into a handle's data or a typed array's bytes, so it ends with `keep.alive` (lowered to MIR `keep_alive`) over those owners, placed after the result has been wrapped or copied: a collection inside the native, inside a program the native re-enters, or inside the result's conversion cannot run a handle's destructor under the native. AOT objects carry each function's encoded stack map in their `bronze_code_range` table, and `bronze_register_code_ranges` hands those maps to brass's code registry when the image loads. A collection triggered from C++ (bronze's allocator, a builtin) walks the native stack from where it is (`HeapConfig::walk_stack_on_host_collection`). C++ frames root their own values with `Rooted<>`, `RootedArgs` and `RootedBlock` on the runtime's C++ shadow stack. On SysV targets that walk follows the frame-pointer chain, so every C++ frame between a compiled frame and an allocation must keep its frame pointer: the runtime builds with `-fno-omit-frame-pointer`, and so must a host whose callbacks allocate.

There is no textual IL front door and no mock runtime. The only input is the IL bronze's compiler builds in memory.

---

## 2. Supported / Translated Construct Matrix

| Category | Bronze IL Instructions | Brass MIR Lowering | Status |
| :--- | :--- | :--- | :--- |
| **Module & Functions** | `module <path>`, `func name(...) -> type [export]` | `Module`, `Function` with typed signatures | Supported |
| **Basic Blocks** | `b0:`, `b1(%0: type, %1: type):` | `BasicBlock`, `add_block_param` | Supported |
| **Constants** | `const.f64`, `const.i32`, `const.bool`, `const.undefined`, `const.null`, `const.bigint` | `build_fconst_f64`, `build_iconst_i32`, `build_iconst_i64` | Supported |
| **Arithmetic** | `add`, `sub`, `neg`, `mul`, `div`, `mod` | `build_add`, `build_sub`, `build_mul`, `build_sdiv`, `build_smod` / `bronze_f64_mod` | Supported |
| **Bitwise Operations** | `and`, `or`, `xor`, `shl`, `shr`, `ushr`, `bitnot` | `build_and`, `build_or`, `build_xor`, `build_shl`, `build_ashr`, `build_lshr` with auto-coercion | Supported |
| **Type Conversions** | `to.int32`, `to.numeric` | `build_fptosi_i32`, `build_sitofp_f64_i32`, identity | Supported |
| **Boxing & Unboxing** | `box.f64`, `box.i32`, `box.bool`, `unbox.f64`, `unbox.i32`, `unbox.bool` | `build_bitcast_i64_f64`, NaN-tag embedding, `build_bitcast_f64_i64` | Supported |
| **Comparisons** | `cmp.lt`, `cmp.gt`, `cmp.le`, `cmp.ge`, `cmp.eq`, `cmp.ne`, `strict.eq`, `loose.eq`, `rel.*` | `build_slt`, `build_sgt`, `build_sle`, `build_sge`, `build_eq`, `build_ne` | Supported |
| **Control Flow** | `jump bX(...)`, `br %cond, bX(...), bY(...)`, `ret %val`, `ret` | `build_br`, `build_br_if`, `build_ret` | Supported |
| **Global Resolution** | `name.resolve "<name>"` | Lowers host symbols (`print`, `console.log`, `print.err`) to tagged host callable descriptors | Supported |
| **Dynamic Calls** | `call.dynamic %callee, %this, <argc>, %args...` | Marshalling through `bronze_call_dynamic_*` dispatcher with JS-compliant number formatting | Supported |
| **Environments & Scope**| `env.create`, `env.get`, `env.set`, `env.get.tdz`, `env.init.tdz` | Heap-allocated lexical environment chains (`BronzeEnv`) | Supported |
| **Closures** | `create.func @fn, <param_count>, %env` | Code pointer + environment pairing (`BronzeClosure`) | Supported |
| **Direct Calls & Prints**| `call @name(...)`, `print %0, ...`, `print.err %0, ...` | Direct internal/external subroutine calls & formatted printers | Supported |
| **Objects & Properties**| `create.object`, `create.array`, `prop.get`, `prop.set`, `elem.get`, `elem.set`, `method.def` | Bronze's inline caches and shapes (the IC layout comes from `il_abi.h`), with objects allocated by bronze's runtime | Supported |
| **Accessor Properties** | `accessor.def %obj, "key", %getter, %setter`, `accessor.def.computed %obj, %key, %getter, %setter` | Lowered to `bronze_accessor_def` and `bronze_accessor_def_computed` runtime descriptors | Supported |
| **Exception Handling**| `handler b<id>`, `throw %val`, `exc.take` | MIR `invoke`, `throw`, `landing_pad` with zero-cost Win64 SEH & SysV DWARF LSDA unwinding | Supported |
| **Coroutines & Async**| `create.async_machine`, `async.start`, `async.await`, `iter.open`, `iter.step`, `iter.value`, `iter.close` | Runtime calls (`bronze_iter_*`, `bronze_async_*`). Generators and async functions arrive already desugared into `.resume` state machines on bronze's heap, so MIR sees ordinary functions | Supported |

---

## 3. Bronze Corpus Verification Coverage

The 38-program corpus lives in `tests/oracle/cases/tiers_*.js` with pinned `.expected` bytes. Like every oracle case, these programs run AOT (with and without inference, plus gc-stress) and under `bronze run`. The `oracle-tiers` ctest (`ctest -L tiers`) also runs each one under `bronze run --tier=0|1|2|auto`, both plain and under gc-stress:
- **01–08**: Core numeric computation, bitwise operations, Collatz, iterative & recursive Fibonacci, Ackermann, Newton sqrt, and prime sieve.
- **09–13**: Closures, loop variable capture, lexical environments, counter closures, and nested currying.
- **14–22**: Arrays, nested accumulation, parameter bounds, overflow handling, vector/matrix math, and bounding boxes.
- **23–24**: Object instantiation, hidden class Shape transitions, and polymorphic inline caching.
- **25–26**: Exception handling with nested `try`-`catch` and `try`-`finally` unwind frames.
- **27–28**: Fibonacci generators (`iter.step`) and multi-stage async promise chains.
- **29–30**: Generational GC allocation churn and On-Stack Replacement (OSR) hot-loop migration.
- **31–33**: GVN-PRE diamond hoisting, loop fusion with array contraction, and AVX2 FMA matrix multiplication.
- **34–37**: Background multi-tier JIT compilation, parallel matrix-vector dispatch, DWARF/CodeView source line mapping, and fuzz-hardened kernels.
- **38**: Tagged stack-map roots: wide argument lists staged through the argv block, construct and method calls, `arguments`, rest params, closures and recursion, all with objects live across allocating calls.

---

## 4. Impedance Mismatches Identified & Resolved

1. **Dynamic Call Edge (`name.resolve "print"` + `call.dynamic`)**:
   - *Issue*: Bronze compiles JavaScript `print(...)` into a dynamic global name lookup (`name.resolve "print"`) followed by a variable-argument `call.dynamic` rather than a static opcode.
   - *Resolution*: Lowered to host symbol descriptors and dynamic dispatchers that unpack NaN-boxed argument payloads and format output identically to Node.js.
2. **ECMA-262 Number Formatting**:
   - *Issue*: C/C++ default `%g` or `%f` prints integer doubles as `1.62412e+06` or `55.0`.
   - *Resolution*: Implemented `std::to_chars`-backed JS-style formatting where integer-valued floats print without exponents or trailing decimals, matching Node.js `ToString(Number)` byte-for-byte.
3. **Lexical Scope & Per-Iteration Capture**:
   - *Issue*: Closures capture mutable outer scopes (including per-iteration variables in `for (let i...)` loops).
   - *Resolution*: Lowered to `BronzeEnv` frame chains and `BronzeClosure` handles passing the environment context as hidden first argument.
