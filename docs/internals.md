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
   │  src/codegen-llvm  — LLVM, gated by BRONZE_WITH_LLVM
   ▼
object file → system linker → exe
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

Two flags exist for embedding a compiled program in a host process rather
than shipping it as an executable. `--emit-obj` stops `build` after object
emission — `-o` names the object file, written exactly where given, and no
linker runs; the host's build links it against bronze's runtime and the
host's own code. `--host-globals <path>` names a manifest (one identifier
per line, `#` comments) of globals the host promises to register with the
runtime before the program runs: each joins the provided-globals set, so a
read lowers to the same `global.get` a builtin's does instead of the
unresolved-name warning and runtime ReferenceError. Both are lowering- and
link-level facts, so `--no-infer` changes nothing about either.

`src/embed` is the host-facing C++ API: run a compiled program in-process,
register host globals, wrap native functions and objects, hold GC-safe
handles across frames. The bro engine's `src/bronze_host/` is a complete
worked example — a browser-shaped global set (`document`, canvas, WebGL2,
timers, rAF) backed by a real engine.

Every emitted object carries `bronze_object_abi_fingerprint`, a hash of
`src/abi/bronze_abi.h` at the compiler's build; the runtime compares it to
its own at program entry (`runtime/abi_guard.h`), so an object adopted by a
host whose runtime speaks a different ABI dies at startup with both values
named — or at link, for an object predating the stamp — instead of reading
garbage through a drifted helper signature.

## Build modes

```
cmake --preset dev          # configure (vcpkg installs doctest, small)
cmake --build --preset dev  # build everything
ctest --preset dev          # run all module tests
```

That is the light configure, and it does not build a compiler that can emit
an executable: the LLVM backend is opt-in, and `tests/oracle` is only defined
when it is on. Working on the front half — lexer, parser, inference, lowering,
IL — that is the loop you want, and its tests are the whole suite.

```
cmake --preset dev -DBRONZE_WITH_LLVM=ON
```

is the other one, and it is what `bronze build`, the oracle suite and the
three.js milestone need. **It is the configure the pre-commit run means.**
Without it `ctest` does not fail, it runs a smaller suite, which is the more
dangerous of the two.

The `llvm` vcpkg feature is provisioned once (`vcpkg` builds it from source,
hours; binary-cached afterwards). Day-to-day work on the front half does not
rebuild or even see it.

## Iteration workflow (the point of this repo layout)

Every compiler component is an isolated static library with its own test
binary and ctest label. The loop for working on one module is:

```
cmake --build --preset dev --target bronze_lex_tests && ctest --preset dev -L lex
```

Rules that keep iteration fast:

- **Heavy deps are opt-in.** The default configure/build never touches LLVM.
- **Scoped tests per change; full `ctest` before a commit.** Not the other
  way around.
- **No module reaches into another's internals.** Dependencies flow through
  the `bronze::<module>` link targets only; the CLI is where modules meet.

## Layout

| Path | Contents |
|---|---|
| `src/support` | Source buffers, spans, diagnostics, and the `--timings` flag the CLI and the LLVM backend both report through |
| `src/lex` | Hand-written lexer (TS core) |
| `src/ast` | AST nodes + visitor + canonical dump |
| `src/parse` | Recursive-descent parser, split by grammar seam: `parser_stmt` (cursor + statements), `parser_expr`, `parser_literal` (escapes, templates, object/array literals), `parser_func` (functions, arrows, classes), `parser_pattern` (destructuring targets), `parser_module` (import/export), `parser_generator` (the desugaring), `parser_strict` (the Directive Prologue and the early errors strict code alone has) |
| `src/modules` | The module graph: specifier resolution, the depth-first load (cycles included — the temporal dead zone is what makes one well defined), and the linker that renames N files' module scopes into one flat namespace so everything downstream still sees a single-file program |
| `src/types` | Type/shape inference over the AST — lattice, flow analysis, shape classes, call-graph signatures, canonical dump. Produces a side table; mutates nothing |
| `src/lower` | AST + inference side table → IL. Split by seam, one file per construct family rather than by size: `lower_infer` (what may be believed), `lower_scope` (closures and env slots), `lower_control` (block-argument SSA), and a file each for the expression kinds (`lower_expr`, `_binary`, `_chain`, `_cond`), the statement kinds (`lower_stmt`, `_switch`, `_try`, `_label`, `_iter_loop`), and the declaration kinds (`lower_object`, `_class`, `_pattern`, `_update`, `_unresolved`) |
| `src/il` | Typed SSA IL: types, module model, canonical printer, verifier |
| `src/codegen` | Backend interface |
| `src/codegen-llvm` | LLVM backend (gated: `BRONZE_WITH_LLVM`): `llvm_abi` (helper declarations from the ABI registry), `llvm_prop` (inline property caches), `llvm_func`/`llvm_ops`/`llvm_arith` (one IL function's body), `llvm_backend` (module in, object file out) |
| `src/abi` | The generated-code ABI (`bronze_abi.h`) and its pure-C compile check — the only place a runtime helper signature is written. Its content hash is the ABI fingerprint (see Embedding above) |
| `src/runtime` | The dynamic value model: NaN-boxing, heap + GC, shapes, objects, arrays, strings, environments. The ABI helpers are `rt_state` (process-wide state and the caches rooted with it), `rt_convert`, `rt_object`, `rt_prop` (property access, split by receiver kind: `rt_prop_primitive` is the one whose answer comes from an intrinsic rather than from the receiver), `rt_iter`, `rt_print`, `rt_members` (what ECMA-262 defines and bronze has not built). Unicode DEFAULT CASE CONVERSION lives here rather than in `src/regex`, because it is a different operation from folding over different data: `unicode_case` is the algorithm and `unicode_case_data_*.cpp` the generated tables |
| `src/json` | The JSON grammar alone (RFC 8259 / ECMA-262 25.5.1): code units in, a tree out. Deliberately not `src/parse` — it exists for what it REFUSES that JavaScript accepts |
| `src/regex` | The RegExp pattern grammar (ECMA-262 22.2.1) and its backtracking matcher, on the same rule as `src/json`: a language of its own inside the source text, with its own parser and its own diagnostics. Reached from `src/lex`, which decides whether a `/` opens a pattern or divides, by what came before it. Its Unicode data — General_Category and simple case FOLDING, which is not the same table as the case CONVERSION `src/runtime` applies — is generated once by `tools/gen_unicode_tables` and checked in as ordinary sources (`unicode_data_*.cpp`); the build never runs generator tooling |
| `src/rt` | The static library compiled output links against |
| `src/embed` | The host-facing C++ embedding API (`tests/embed` holds its suite): run a compiled program in-process, register host globals, wrap native functions and objects, hold GC-safe handles across frames. Depends on the runtime and only calls it — the runtime never learns it exists |
| `src/cli` | `bronze` driver (`lex`, `parse`, `types`, `il`, `build`, `version`) |
| `tests/<module>` | doctest suites, one per module |
| `tests/oracle` | Differential cases with pinned `.expected` stdout — see `tests/oracle/README.md`. A case is `cases/<name>.js`, or `cases/<name>/main.js` plus what it imports |
| `tools` | Generators whose OUTPUT is committed. Run by hand, never by the build, so that no build step depends on a language bronze does not already require |
