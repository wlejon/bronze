# il2mir: bronze IL to brass MIR

## 1. Overview
`src/codegen-brass/il2mir/` lowers bronze's IL to brass MIR. It lives in bronze, and brass knows nothing about it. The pipeline is `bronze::il::Module` → `il_to_brass_ast` (`codegen-brass/il_to_brass_ast.*`) → `il2mir::BronzeModuleAST` → `il2mir::translate_bronze_ast` → `brass::Module`. `BrassBackend` then runs that module through brass's pass pipeline (`il2mir::pass_pipeline_options`) for AOT objects, the JIT, or the tiered engine.

Everything is in the top-level namespace `il2mir`, which pulls in `brass` (`il_ast.h`). The value/TLS/IC layout constants come from the bronze ABI header (`il_abi.h`), not from copies.

The lowered code calls the bronze runtime (`bronze_*`, `brass_gc_*`) by name. Brass resolves those names through its generic host-symbol hook, `brass/runtime/host_symbols.hpp`. `codegen-brass/brass_symbol_registration.cpp` installs a `HostSymbolProvider` that binds the real bronze ABI (the `BRONZE_ABI_FUNCTIONS` X-macro), and the math and parallel helpers into every engine a brass tier creates: the fast interpreter, the baseline JIT, the tier-2 installer JIT, deopt and the reference interpreter.

Bronze's objects live on brass's collector. Each thread's `bronze::Heap` (`runtime/heap.h`) owns one `brass::gc::Heap` and binds it as the thread's current heap, so every object bronze-run code creates (objects, arrays, environments, closures, strings, and the brass coroutine frames of suspended generators and async functions) is a brass heap object whose payload begins with bronze's `HeapObjectHeader`. Bronze registers the object layouts and trace functions (`runtime/heap_trace.cpp`), and the brass interpreters on the thread register their frames with that same heap. The inline allocation paths (`il_alloc_lowering.cpp`) bump the thread's TLS allocation window, which is bound as the brass heap's allocation buffer, and write brass's cell header (`gc_cell_header` in the TLS block) in front of each bronze header. Bronze's `brass_gc_write_barrier` (`runtime/gc.cpp`) is the process's only definition of that name: it strips the NaN-box tag from the object operand and takes brass's card barrier on whichever of the thread's heaps holds the object, overriding brass's `brass_default_gc_write_barrier` through the symbol table.

Compiled code roots its JS values through brass stack maps, not through anything bronze keeps. A `Dynamic` value crosses every compiled-function boundary as MIR `tagged` (`lower_abi_type`): SSA results, block params, and the params and returns of functions with a body. Native imports keep raw `i64` signatures. Arithmetic, tag tests and runtime calls work on `i64`, so every use unboxes with `bitcast.i64.tagged` (`get_val_by_id`) and every result boxes back with `bitcast.tagged.i64` (`set_inst_result`). `unbox_tagged` folds a box that sits in the same block with no call or safepoint between it and the use, and `ensure_type` folds `box(unbox(x))`, so straight-line code keeps its values in registers. Brass records every tagged value live at a safepoint, in a register, a spill slot or an interpreter register, as a stack-map root whose `value` kind is `Tagged`. The collector updates it in place and keeps its NaN-box tag. Arguments passed through memory (a construct, a method call, a super call, or a dynamic call with more than 16 arguments) go through one `alloca.tagged` argv block per function, sized to the widest such call. The block is zeroed, and each of its words is a root at every safepoint. A value is a root only while it is live. A native call hands the native raw pointers into a handle's data or a typed array's bytes, so it ends with `keep.alive` (lowered to MIR `keep_alive`) over those owners, placed after the result has been wrapped or copied: a collection inside the native, inside a program the native re-enters, or inside the result's conversion cannot run a handle's destructor under the native. AOT objects carry each function's encoded stack map in their `bronze_code_range` table, and `bronze_register_code_ranges` hands those maps to brass's code registry when the image loads. A collection triggered from C++ (bronze's allocator, a builtin) walks the native stack from where it is (`HeapConfig::walk_stack_on_host_collection`). C++ frames root their own values with `Rooted<>`, `RootedArgs` and `RootedBlock` on the runtime's C++ shadow stack. The walk steps through the C++ frames between two compiled frames by their unwind information (brass's `native_unwind.hpp` off Windows), so neither the runtime nor a host has to keep frame pointers.

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
| **Exception Handling**| `handler b<id>`, `throw %val`, `exc.take` | MIR `invoke`, `throw`, `landing_pad`; zero-cost unwinding through brass's personalities (section 5) | Supported |
| **Coroutines & Async**| `coro.start`, `coro.suspend`, `coro.mode`, `iter.delegate`, `iter.open`, `iter.step`, `iter.value`, `iter.close`, `async_iter.*` | Brass coroutines: `coro_create` in the stub, `coro_suspend` / `coro_resume_mode` in the body, split by brass's coroutine lowering (section 6); iteration through `bronze_iter_*` / `bronze_async_iter_*` | Supported |

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

---

## 5. Exceptions

A JS throw is a brass raise; nothing stores a pending exception for a caller to test, and code that does not catch pays nothing on its normal path (`il_lowering_eh.cpp`):

- A `throw` inside a `try` is a branch to its handler with the value. One outside a `try` is a MIR `throw`, which leaves the function.
- A call inside a `try` is a MIR `invoke`. Its unwind edge goes to one `landing_pad` per handler, which receives the thrown value in the return register and branches to the handler with it. A call outside a `try` is a plain call with nothing after it.
- `exc.take` reads the value the handler was given.

The value crosses C++ as exactly one type: `brass::runtime::BrassException` (`runtime/exception.h`). The runtime raises with the `[[noreturn]]` `rtThrow*` helpers, which throw one; a host native or sibling `_api` function throws one the same way. The C++ frames between the raise and the nearest compiled `invoke` unwind as they do for any C++ exception, `Rooted<>` destructors included. brass's personality routine then lands the exception at the pad: `brass_seh_personality` on Windows, named by every compiled function's `.xdata`; `brass_sysv_personality` elsewhere, named by the `.eh_frame` CIE of every compiled function with pads, which finds the pad in the function's LSDA or brass's JIT registry. A raise from compiled code (MIR `throw`) whose pad is in a compiled caller is found by brass's frame walk and jumped to directly. Every tier lands at the same pads: the interpreters' `invoke` catches the C++ exception, and tier-up, OSR and deopt keep the handler blocks.

C++ that calls compiled code, or catches for a reason of its own, uses `rtTryCatch(body, thrown)`: it runs `body` and answers whether it threw, storing the value in `thrown` after the handler has returned, so a collection during the body cannot leave it stale.

At the embed boundary (`embed.h`) an entry point that returns a result reports a throw in it: `call`, `construct`, `parseJson` and the property getters return a `CallResult` whose `thrown` is set, and `catchThrow(body)` does the same for any host code that calls into JS. `setElement` drops the write when a setter throws. `deleteProperty` and `createTypedArray` let the exception propagate to the host. An uncaught throw at the top of a module leaves `rtRunModuleEntry`, which reports it with its stack and exits the process with status 1; `BrassTieredProgram::run` lets it leave as the `BrassException` it is. A promise job's throw rejects its promise, and an unhandled rejection is reported when the job queue drains.

`Error.prototype.stack` and `Error.captureStackTrace` walk the native stack by its unwind information when the error is made (`runtime/stack_trace.cpp`), so a stack reads the same whether the error is thrown, caught or never thrown.

---

## 6. Generators and async functions

Generators, async functions, async generators and a top level that awaits are brass coroutines at every tier (`lower/lower_generator.cpp`, `il_lowering_coro.cpp`, `runtime/coro.cpp`).

- **One body, suspensions in the middle.** src/lower lowers such a function as an ordinary body in which each `yield` / `await` is `%sent = coro.suspend <kind> %v` followed by `coro.mode`, and a branch on the mode: next continues with `%sent`, throw is a `throw %sent` at that point, return is a `return %sent` at that point. `throw()` and `return()` therefore reach the enclosing `catch` and `finally` blocks through the same lowering as any other throw or return, and bindings stay in SSA. `yield*` is a loop around `iter.delegate` (`runtime/iterator_delegate.cpp`), which steps the inner iterator with the resumption's mode.
- **Stub and body.** `splitCoroutineBody` moves the blocks into `<name>.body` (coroKind set) and leaves `<name>` as the stub `coro.start <kind> @<name>.body(params)`. il2mir lowers the stub to MIR `coro_create` (whose frame allocation is a GC point, so its tagged arguments are roots across it) and `bronze_coro_start(kind, frame)`, which answers the generator object or the promise. The body takes the frame as a leading `gcref` parameter.
- **Suspension.** `coro.suspend` is MIR `coro_suspend` with a state id whose low two bits are the kind (`BRONZE_ABI_SUSPEND_KIND_MASK`: start, yield, await, delegate). The resumer passes the address of a rooted cell holding the sent value as the resume value, and the body loads it after the suspend, so the value is current however much the resumer allocated. `coro.mode` is MIR `coro_resume_mode`. A body is resumed from C++, which does not carry the pinned TLS register, so the body fetches the block (`bronze_tls_block_addr`) at entry and after every suspend.
- **Frame lowering.** il2mir runs `brass::lower_coroutines` on the module before its optimization pipeline: until the split, a suspend is an ordinary instruction to the passes, which would move a frame read or an unboxed reference across it. The lowering spills every value live across a suspend into the frame and records the frame layout on the body; allocas must be in the entry block. AOT bodies allocate their frames through `brass_coro_create` (a `BRONZE_ABI_BRASS_EXPORTS` entry of the shared runtime); JIT tiers through the body's CoroBody descriptor. A tier-0 body resumed with no interpreter running starts one through the program's pipeline.
- **Ownership.** A new frame is one of brass's roots until `bronze_coro_start` hands it to its owner (the generator object, the async function's machine, the async generator), which `rtCoroAdopt` unroots; from then on the owner traces it. The heap is bound on the thread's first entry into generated code (`bronze_tls_enter`), before any frame can be created.
- **Drivers.** The runtime keeps the protocol state: `builtin_generator.cpp` (27.5.3), `builtin_async.cpp` (27.7.5) and `builtin_async_generator.cpp` (27.6.3) resume through `rtCoroResume` and subscribe awaits through `rtCoroAwait`, which subscribes the awaited promise itself when it is one (one reaction job per await).
- **Not yet:** async stack traces (the brass awaiter links are not set, so a stack captured in a resumed body ends at the body's frame), and leaving a for-await loop early does not await the iterator's `return()` result (`tests/oracle/cases/blocked/for_await_close_awaits_return.js`).

---

## 7. Speculation sites

Under the tiered pipeline (`brass_tiered_engine.cpp`, every tier except standalone `--tier=2`), a program is lowered with a `SpecFeedback` (`il2mir/il_speculation.h`). The program owns it because the lowered code increments its counters in place. Each inline fast path whose test can fail at run time is a **site**: `br_if %hit, fast, slow`, where `slow` counts the miss and then calls the generic helper.

| Site | Fast path | Slow path | Armed when |
|------|-----------|-----------|------------|
| dynamic `+ - * /` (`il_lowering_ops.cpp`) | both operands Numbers: f64 op, NaN-canonicalized box | `bronze_dynamic_*` | the slow path never ran |
| `prop.get` (`lower_prop_get_mono`) | plain object whose shape is the site's way 0, own data slot, inline or overflow | `bronze_prop_get` | at most 2 misses (the cold IC fills) |
| `prop.set` (`lower_prop_set_mono`) | the same test, then store plus write barrier for a reference | `bronze_prop_set` | at most 2 misses |

With feedback, every keyed get and set site gets the inline path, not only the ones inference proved monomorphic (`is_mono`). Set sites stay out-of-line while a shape census counts every write.

**Guards.** In a function that can deoptimize, each site also has a guard in front of its branch: `guard 1, "bronze.spec"` with the site's number as resume id, `slow` as the resume target, and every value live into `slow` as its state. It is always true in Tier 0 and Tier 1. When a function tiers up, `apply_tier2_speculation` (brass's tier-2 front pass, `docs/speculation_and_deopt.md` §4b in brass) reads the counts. An armed site gets `%hit` as its guard condition and its branch pinned to `fast`, so a miss in tier-2 code deoptimizes and resumes the call in Tier 0 at `slow`. Other sites lose the guard and keep the branch. Either way the copy drops its resume points. Repeated deopts invalidate the code, and brass reoptimizes up to three times with the counts the failures added.

A function can deoptimize only if nothing its slow paths read would be wrong in the fresh Tier-0 frame a resume enters. That excludes coroutine bodies and functions with an argv block (any method or n-ary call), because the block is an alloca in the tier-2 frame. Those functions keep the inline paths as plain branches.

Seams: `BRONZE_NO_SPECULATION=1` lowers without feedback (the pre-speculation lowering). `BRONZE_SPEC_NO_GUARDS=1` keeps sites and counts but emits no guards. `BRONZE_SPEC_NO_ARM=1` emits guards and never arms them. `BRONZE_SPEC_TRACE=1` prints armed/total per tier-2 copy. `oracle-tiers-deopt-stress` and `oracle-tiers-deopt-stress-3` run the tiers suite with `BRASS_DEOPT_STRESS` at 1 and 3, and `tiers_39_speculation_deopt` exercises each site kind going polymorphic after tier-up.
