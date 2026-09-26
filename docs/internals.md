# bronze internals

This is the pipeline, the repository layout, inference and its off switch, embedding,
and the iteration workflow the repo layout exists to serve.

## The pipeline

```
source.js
   │  src/lex      — tokens (hand-written lexer, hard errors on unknown input).
   │                 A `/` is a division or the start of a pattern depending on
   │                 what preceded it; when it is a pattern, src/regex owns the
   │                 grammar inside it
   ▼
tokens
   │  src/parse    — recursive descent, consumes ALL input or errors
   ▼
AST (src/ast)      — plain structs + visitor; canonical s-expr dump
   │
   ├─▶ src/types   — inference over the AST: flow-sensitive types per
   │                 binding, shape classes per object site, and signatures
   │                 joined over the call graph. Produces a SIDE TABLE
   │                 (`types::InferenceResult`) keyed by AST node; mutates
   │                 nothing. Canonical dump: `bronze types <file>`
   │  src/lower    — AST + side table → IL. The only consumer of the table,
   │                 and the only place that knows it can be absent, which
   ▼                 is all `--no-infer` is
IL  (src/il)       — typed SSA, canonical text form. Native types where
   │                 inference PROVED them; `dynamic` everywhere else, which
   │                 is the sound fallback, not a failure
   ▼
Backend (src/codegen interface)
   │  src/codegen-brass — Brass native backend:
   │                      • AOT mode: object → brass's own image writer → dll / so / dylib
   │                        (an executable is that module beside a copy of src/host)
   │                      • JIT mode: in-memory compilation → executable RAM (BrassJitProgram)
   ▼
object file / executable RAM
```

Dependency edges point downward only: support ← lex ← parse; `ast` is a peer
of `lex`; `il` depends only on support; `types` depends on `ast` and support
and **must never learn about the IL**; `lower` depends on `ast`, `il` and
`types`. The CLI is the composition root, and where inference is run and its
result handed to lowering.

Every stage owns a canonical, deterministic text form, and tests compare
bytes. That discipline is non-negotiable here — it is what catches the class
of bug that silently changes meaning.

## Inference, and `--no-infer`

bronze types nothing by declaration. `src/types` analyses the AST and proves
what it can — a binding's type at a program point, an object site's shape,
a function's signature joined over its call sites — and lowering emits a
native type only where there is a proof. Everything else is `dynamic`, which
is the designed sound fallback and never a diagnostic. `bronze types <file>`
prints what was proven.

`dynamic` is the fallback and never the substrate. That distinction is the
whole architecture: a value is boxed because nothing proved it, rather than
boxed by default with proofs carving out exceptions. It is what makes
`function fib(n) { ... }` lower to `func fib(%0: f64) -> f64` with a direct
typed call rather than to a boxed argument through the uniform dynamic
convention.

A TS annotation is an **untrusted hint**. Inference never reads one: if it
proves the same type, the native path is taken *because of the proof*; if it
proves something else or proves nothing, the annotation is discarded, the
value stays `dynamic`, and you get a warning naming both. `function
f(x: number)` reached with a string compiles and runs as JavaScript.
Annotations are read in TypeScript's spellings (`string`, `boolean`,
`number`, `any`, `unknown`, …) as well as bronze's own IL names; text bronze
cannot read at all is a hard error naming the vocabulary, not a silent skip.

`--no-infer` forces every inferred type to `dynamic` and lowers on the
uniform dynamic convention. The annotation warnings go quiet with it —
nothing is provable in that mode, so they would say only that the switch is
on — while the annotation *error* still fires, because unreadable text is a
fact about the source. It exists for one reason: it is the **bisection
seam** for any miscompile inference is suspected of causing — if a program
is right with it and wrong without it, the analysis is at fault, and if it is
wrong with it too, the analysis is not. It is a ratchet rather than a comfort
blanket because the oracle suite compiles and runs *every* case both ways and
requires the same pinned bytes from both: a case only inference gets right
means the dynamic path is unsound, and a case only `--no-infer` gets right
means inference is.

## Embedding

Four flags exist for embedding a compiled program in a host process rather
than shipping it as an executable. `--emit-obj` stops `build` after object
emission — `-o` names the object file, written exactly where given, and no
linker runs; the host's build links it against bronze's runtime and the
host's own code. `--host-globals <path>` names a manifest (one identifier
per line, `#` comments) of globals the host promises to register with the
runtime before the program runs: each joins the provided-globals set, so a
read lowers to the same `global.get` a builtin's does instead of the
unresolved-name warning and runtime ReferenceError. `--entry-symbol <name>` names the object's exported entry point,
which defaults to `bronze_main`. `--emit-shared` writes a loadable module
instead of an executable (below). All four are lowering- and link-level facts,
so `--no-infer` changes nothing about any of them.

An object exports exactly four symbols — its entry, its ABI stamp, its
host-globals manifest and its native import table, the last three named after
the entry — plus its code-range table (`<entry>_code_ranges` and
`<entry>_code_range_count`, `bronze_object_*` for the default entry), which a
host registers for stack walks; everything else it defines is internal. That is
what lets a host link **more than one** compiled module into one image and
enter them in turn. The runtime is shared, and what each module owns privately
is its inline-cache table, its key remap, its global cache, its
function-singleton slots and its module-environment cell; all of them are
arrays in the module's own data, and the Value-holding ones are handed to the
collector as root spans at module init. Property-key identity is the one thing
that must be process-wide, so key registration INTERNS: two modules that both
mention `position` are handed one id, and a module's own numbering survives
only in its remap. `tests/two_module` is the worked example — two objects, two
entry symbols, one runtime, with cross-module prototype chains, closures and
exceptions pinned byte-for-byte.

### Loading a module instead of linking one

`--emit-shared` writes the object as a DLL/.so/.dylib importing the SHARED
bronze runtime (`bronze_runtime_shared`, `cmake/bronze_shared_runtime.cmake`).
No linker runs: brass's own image writers (`AotLinker`) lay the image out in
process, every symbol the object leaves undefined becomes an import from the
runtime by name — after `src/cli/link.cpp` has checked each one against the
ABI registry, so a name the runtime does not export is a build error rather
than a loader's — and ELF and Mach-O modules carry their own directory and
the runtime's as run-time search paths. A `bronze build` executable is the
same module beside a copy of the program host (`src/host/host_main.cpp`),
which opens the module named after itself and runs it through
`embed::runEntry`, with the runtime library staged into the same directory.
A host opens a module at run time and learns everything it needs from four exported symbols, all named
after the entry: `<entry>`, `<entry>_abi_fingerprint`, `<entry>_host_globals`
— the `--host-globals` manifest the module was compiled against, as a count
and a run of NUL-terminated names — and `<entry>_native_imports`, the table
of host natives the module calls (below). `src/abi/bronze_abi.h` is the
contract for all four, including the layouts. A loader checks the stamp
against `embed::abiFingerprint()` before calling anything, diffs the manifest
against what it has registered, binds the import table
(`embed::bindNativeImports`, which names every native it cannot fill), and
enters the module through `embed::runEntry`.

### Host natives: registered by the host, bound at load time

A host exposes C functions to programs with `embed::registerNative(jsPath,
fn, NativeSignature)` — `bro.mesh.box` as a function, `bro.time.scale` as a
namespace property (a getter and a setter), `bro.ai.AIAgent` as a class whose
constructor returns a `void*` the runtime wraps into a handle, with methods,
getters and setters under `bro.ai.AIAgent.<member>`. The signature vocabulary
(`src/abi/bronze_native_type.h`) is closed and checked at registration: `void
f64 i32 bool str dynamic`, the typed arrays `f32[] f64[] i32[] u8[] u16[]
u32[] i8[] i16[]` (as a parameter, crossing as a `(T*, uint32_t)` pair valid
for the call; as a return, a `void` C function with one extra trailing
`bronze_native_buffer* out` it fills — `release == NULL` and the runtime
copies `length` elements into a fresh JS-owned array, `release != NULL` and
the array is a zero-copy view over `data` owing `release(ctx)` when the
buffer is collected), and a registered class's path (a handle of that class,
crossing as the `void*` its constructor returned, with a one-word tag compare
at the call site and a TypeError naming the class for anything else). An
unknown spelling is a refused registration, never `dynamic`. A bare name that
is already a host global is refused too, and the reverse registration is
fatal — the two registries never shadow each other.

The lowering (`src/lower/lower_native.cpp`) turns `bro.mesh.box(w, h)`,
`new bro.ai.AIAgent(1)`, `agent.move(dx)` on a binding it saw constructed
(or a captured `const`), `agent.hp`, `agent.hp = v` and `bro.time.scale` into
direct calls with the scalar coercions done in JS order, the pointers taken
last (a str is copied into a per-call scratch, a typed array's bytes are
checked for kind and detachment), and the result wrapped back — a class-typed
return becomes a handle born on the class's prototype, owing the class's
destructor. A typed-array return is sequenced by the import thunk
(`src/codegen-brass/brass_backend.cpp`), the same code for the JIT and an
emitted object: `bronze_native_buffer_slot()` pushes a zeroed per-thread
descriptor, the native is called with its address as the trailing argument,
and `bronze_native_buffer_wrap(kind)` pops it and answers the view (copy or
transfer by `release`). A native that throws unwinds its thunk past the wrap,
so each descriptor records its thunk's frame, and the next slot or wrap on the
thread drops the descriptors whose thunk is gone, running the `release` of one
the native had filled before it threw.

No native symbol is ever resolved by a linker. Each direct call goes through
a slot of the module's import table, `<entry>_native_imports`: `u32 count;
u32 namesOffset; u64 slots[count];` then `count` × `"<kind> <path>\0"
"<signature>\0"` (or `"class <path>\0" "\0"` for the slot that carries a
class's tag). Function slots start as the address of `bronze_native_unbound`
(a trap that names the situation), class slots as 0. The JIT (`evalScript`)
reads the thread's registry directly and binds the program before it runs;
`bronze build --native-manifest <file>` reads the same registry printed by
`embed::writeNativeManifest` (JSON: `{"version": 1, "natives": [{path,
kind, class?, returns, returnClass?, params, signature}]}`), and the module's
entry binds its own table as its first instruction — fatal, listing every
missing native, if the loader did not bind it first. `tests/native` is the
worked example of both paths, including the refusal of a module compiled
against a native nobody registered.

One runtime in the process is the whole point, and it is why a missing shared
runtime is a diagnosed error rather than a fall back to the static archives: a
second copy would mean a second heap, and a value handed from a loaded module
to its host would be an address in a heap the other collector is free to
reuse. Two boundaries meet here and they have different rules — the C ABI is
fingerprint-checked and safe across compilers, while the C++ embed API needs
the host and the runtime built by the same compiler against the same C runtime
(`src/embed/embed.h` states both). The shared runtime's C export list is
generated from the ABI registry at configure time
(`cmake/bronze_abi_exports.cmake`), so a new `X(...)` line is still the only
edit a new helper needs. `tests/shared_load` is the worked example: a module
compiled with `--emit-shared`, opened by a host linked only against the shared
runtime, plus a fake module whose wrong stamp must be refused before its entry
runs.

`src/embed` is the host-facing C++ API: run a compiled program in-process,
register host globals, wrap native functions and objects, hold GC-safe
handles across frames (`Persistent`, RAII `HandleScope`, `EscapableHandleScope`, `Local<T>`).
The bro engine's `src/bronze_host/` is a complete
worked example — a browser-shaped global set (`document`, canvas, WebGL2,
timers, rAF) backed by a real engine.

Every emitted object carries `bronze_object_abi_fingerprint`, a hash of
`src/abi/bronze_abi.h` at the compiler's build; the runtime compares it to
its own at program entry (`runtime/abi_guard.h`), so an object adopted by a
host whose runtime speaks a different ABI dies at startup with both values
named — or at link, for an object predating the stamp — instead of reading
garbage through a drifted helper signature.

### Threads

The runtime is per-thread: every mutable table is `thread_local`, built
lazily on first touch, so a worker thread that simply calls in owns a fresh,
fully independent runtime — no attach step, no locking, and no teardown (a
thread's runtime is deliberately leaked; pool workers rather than churning
them). Values, Persistents and everything reachable from them are bound to
the thread that made them; cross-thread data travel is the host's job, by
serializing on one side and re-creating on the other.

A module image is NOT thread-bound: one image runs on any number of threads
at once. Everything compiled code writes that can hold a heap reference —
IC table, global cache, template cells, environment cell, native import
table — is laid out as one contiguous run of the module's `.data`,
`[__bronze_instance, __bronze_instance_end)`, and that run is per THREAD.
The entry calls `bronze_module_instance(slot, begin, end)`: the first thread
(the image's home thread) gets delta 0 and uses the image's own bytes, and
the runtime snapshots the run while it is still pristine; every later thread
gets a 64-aligned heap copy of that snapshot. The answer is a byte delta,
kept in a per-thread array the TLS block publishes (`module_deltas`, indexed
by the image's slot id in the shared `__bronze_module_slot` cell), and every
table address generated code forms is the image address plus the calling
thread's delta — loaded once per function from the pinned TLS register. The
key remap stays shared: it holds process-wide interned ids, identical from
every thread. The runtime-side registrations of those tables are
per-thread, so each thread's collector updates only its own copy. The one
rule left is that a thread runs the entry before it calls any of the
module's functions. An in-process program (`src/eval`) in a pipeline tier
lays its data image out the same way, so one compiled program runs on many
threads (`EvalOptions::shareAcrossThreads` compiles it once for all of
them); only an explicit `Tier2_Optimized` program is compiled without the
delta and runs on one thread. `src/embed/embed.h` states the full contract;
`tests/threaded_modules` is the worked example — two compiled modules on two
threads concurrently, and one image entered on two threads at once, each
hammered under per-allocation collection.

## In-Memory Dynamic Execution and JIT (`src/eval`)

Bronze provides native in-memory execution and JIT evaluation without writing files to disk or invoking external linkers. The `src/eval` subsystem compiles JavaScript source code or files to brass MIR and runs them through the tiered engine (`src/codegen-brass/brass_tiered_engine.h`): interpreted first, with hot functions compiled to baseline and then optimized code, and hot loops moved into optimized code mid-loop (OSR), all compiled on brass's process-wide compile pool while the program runs. The optimizer runs per function as it tiers up, not over the whole program before it starts. The whole-program optimizing JIT (`BrassBackend::compileToJit`) remains as an explicit tier.

Key capabilities:
- **Zero-disk dynamic evaluation:** `evalScript` compiles and runs code strings in RAM, returning an `embed::CallResult` or `Value`.
- **In-memory file execution:** `evalFile` resolves full module graphs and executes them directly via JIT.
- **Dynamic builtins:** `eval(...)` and `new Function(...)` (as well as generator and async functions) compile dynamically in-memory via automatic hooks (`installDefaultDynamicHooks`).
- **CLI commands:** `bronze run <file>` runs scripts and module graphs in memory; `bronze eval <code>` (or `-e <code>`) evaluates expressions or statements directly from the command line.

For the complete architectural design and API specification, see [`docs/dynamic-eval.md`](docs/dynamic-eval.md).

## Build modes

```
cmake --preset dev          # configure (vcpkg installs doctest, small)
cmake --build --preset dev  # build everything
ctest --preset dev          # run all module tests
```

The default build compiles Bronze with the Brass native backend (`src/codegen-brass`),
enabling `bronze build`, the oracle suite, and the milestone tests out of the box.

## Iteration workflow (the point of this repo layout)

Every compiler component is an isolated static library with its own test
binary and ctest label. The loop for working on one module is:

```
cmake --build --preset dev --target bronze_lex_tests && ctest --preset dev -L lex
```

Rules that keep iteration fast:

- **Brass native backend.** Native object emission uses Brass.
- **Scoped tests per change; full `ctest` before a commit.** Not the other
  way around.
- **No module reaches into another's internals.** Dependencies flow through
  the `bronze::<module>` link targets only; the CLI is where modules meet.

## Layout

| Path | Contents |
|---|---|
| `src/support` | Source buffers, spans, diagnostics, and the `--timings` flag the CLI and backend both report through |
| `src/lex` | Hand-written lexer (TS core) |
| `src/ast` | AST nodes + visitor + canonical dump |
| `src/parse` | Recursive-descent parser, split by grammar seam: `parser_stmt` (cursor + statements), `parser_expr`, `parser_literal` (escapes, templates, object/array literals), `parser_func` (functions, arrows, classes), `parser_pattern` (destructuring targets), `parser_module` (import/export), `parser_generator` (the desugaring), `parser_strict` (the Directive Prologue and the early errors strict code alone has) |
| `src/modules` | The module graph: specifier resolution, the depth-first load (cycles included — the temporal dead zone is what makes one well defined), and the linker that renames N files' module scopes into one flat namespace so everything downstream still sees a single-file program. A dynamic `import()` is part of that graph rather than a runtime lookup: a string-literal specifier becomes `Promise.resolve(<namespace>)`, and a template literal with a static head and a tail ending in a module extension becomes a GLOB over the one directory the head names — every module matching joins the graph and the call becomes a lookup in a table of exactly those specifiers, rejecting a string the table does not hold. A specifier bronze cannot read either way (a variable; `../../${path}`, whose interpolation is bounded by nothing) is warned about at the call site and left to the runtime: the host's dynamic-import hook (`embed.h setDynamicImportHook`, handed the specifier and the importer's URL) answers it, or the call rejects. A constant specifier that names a directory or a non-module file is a compile error at the import, never a parse of the wrong file |
| `src/types` | Type/shape inference over the AST — lattice, flow analysis, shape classes, call-graph signatures, canonical dump. Produces a side table; mutates nothing |
| `src/lower` | AST + inference side table → IL. Split by seam, one file per construct family rather than by size: `lower_infer` (what may be believed), `lower_scope` (closures and env slots), `lower_control` (block-argument SSA), and a file each for the expression kinds (`lower_expr`, `_binary`, `_chain`, `_cond`), the statement kinds (`lower_stmt`, `_switch`, `_try`, `_label`, `_iter_loop`), and the declaration kinds (`lower_object`, `_class`, `_pattern`, `_update`, `_unresolved`) |
| `src/il` | Typed SSA IL: types, module model, canonical printer, verifier |
| `src/codegen` | Backend interface |
| `src/codegen-brass` | Brass native backend: `brass_backend.cpp` (AOT object emission), `brass_jit.cpp` (in-memory JIT execution via `brass::codegen::JitExecutionEngine`) |
| `src/eval` | In-memory dynamic JavaScript execution and JIT evaluation: `evalScript`, `evalFile`, `evalFunction`, `installDefaultDynamicHooks`. See `docs/dynamic-eval.md` |
| `src/abi` | The generated-code ABI (`bronze_abi.h`) and its pure-C compile check — the only place a runtime helper signature is written. Its content hash is the ABI fingerprint (see Embedding above) |
| `src/runtime` | The dynamic value model: NaN-boxing, heap + GC, shapes, objects, arrays, strings, environments. The ABI helpers are `rt_state` (process-wide state and the caches rooted with it), `rt_convert`, `rt_object`, `rt_reflect` (28.1, whose members are the internal methods by name and so are forwards into the funnels `rt_object` and `rt_prop` already own), `rt_prop` (property access, split by receiver kind: `rt_prop_primitive` is the one whose answer comes from an intrinsic rather than from the receiver, `rt_prop_map` the one whose named properties are a side object beside its entries), `rt_iter`, `rt_print`, `rt_members` (what ECMA-262 defines and bronze has not built). Unicode DEFAULT CASE CONVERSION lives here rather than in `src/regex`, because it is a different operation from folding over different data: `unicode_case` is the algorithm and `unicode_case_data_*.cpp` the generated tables |
| `src/json` | The JSON grammar alone (RFC 8259 / ECMA-262 25.5.1): code units in, a tree out. Deliberately not `src/parse` — it exists for what it REFUSES that JavaScript accepts |
| `src/regex` | The RegExp pattern grammar (ECMA-262 22.2.1) and its backtracking matcher, on the same rule as `src/json`: a language of its own inside the source text, with its own parser and its own diagnostics. Reached from `src/lex`, which decides whether a `/` opens a pattern or divides, by what came before it. Its Unicode data — General_Category and simple case FOLDING, which is not the same table as the case CONVERSION `src/runtime` applies — is generated once by `tools/gen_unicode_tables` and checked in as ordinary sources (`unicode_data_*.cpp`); the build never runs generator tooling |
| `src/rt` | The static library an `--emit-obj` host links against, with the `main` of a statically linked program |
| `src/host` | The program host `bronze build` copies beside every module it writes: loads the module named after itself against the shared runtime |
| `src/embed` | The host-facing C++ embedding API (`tests/embed` holds its suite): run a compiled program in-process, register host globals, wrap native functions and objects, hold GC-safe handles across frames (`Persistent`, RAII `HandleScope`, `Local<T>`). Depends on the runtime and only calls it — the runtime never learns it exists |
| `src/cli` | `bronze` driver (`run`, `eval`, `lex`, `parse`, `types`, `il`, `build`, `version`) |
| `tests/<module>` | doctest suites, one per module |
| `tests/oracle` | Differential cases with pinned `.expected` stdout — see `tests/oracle/README.md`. A case is `cases/<name>.js`, or `cases/<name>/main.js` plus what it imports |
| `tools` | Generators whose OUTPUT is committed. Run by hand, never by the build, so that no build step depends on a language bronze does not already require |
