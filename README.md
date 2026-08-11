# bronze

AOT compiler for JavaScript — real-world, untyped JS (three.js is the bar).
Native code with native data layouts wherever analysis can prove them;
`dynamic` representation where it cannot. TypeScript annotations, when
present, are optimization hints — never required, never trusted for
correctness.
C++ implementation, own typed SSA IL, LLVM backend. Sibling of `bro`
(engine) and `brokit`.

bronze exists because its predecessor (`broc`, TypeScript implementation,
QuickJS object model everywhere) proved the pipeline discipline but landed
~50x slower than node on compiler workloads — the value representation was
the wrong foundation. See `docs/0001-foundation.md` for the full rationale
and the lessons carried over.

## Build

Prereqs on this machine: CMake ≥ 3.24, Ninja, clang-cl (LLVM toolchain in
`C:\Program Files\LLVM`), vcpkg at `D:\vcpkg` (auto-detected; override with
`VCPKG_ROOT` or `-DCMAKE_TOOLCHAIN_FILE`).

```
cmake --preset dev          # configure (vcpkg installs doctest, small)
cmake --build --preset dev  # build everything
ctest --preset dev          # run all module tests
```

## Iteration workflow (the point of this repo layout)

Every compiler component is an isolated static library with its own test
binary and ctest label. The loop for working on one module is:

```
cmake --build --preset dev --target bronze_lex_tests && ctest --preset dev -L lex
```

Rules that keep iteration fast:

- **Heavy deps are opt-in.** The default configure/build never touches LLVM.
  The `llvm` vcpkg feature is provisioned once (`vcpkg` builds it from
  source, hours; binary-cached afterwards) and enabled with
  `-DBRONZE_WITH_LLVM=ON`. Day-to-day work does not rebuild or even see it.
- **Scoped tests per change; full `ctest` before a commit.** Not the other
  way around.
- **No module reaches into another's internals.** Dependencies flow through
  the `bronze::<module>` link targets only; the CLI is where modules meet.

## Inference, and `--no-infer`

bronze types nothing by declaration. `src/types` analyses the AST and proves
what it can — a binding's type at a program point, an object site's shape,
a function's signature joined over its call sites — and lowering emits a
native type only where there is a proof. Everything else is `dynamic`, which
is the designed sound fallback and never a diagnostic. `bronze types <file>`
prints what was proven.

A TS annotation is an **untrusted hint** (docs/0001 decision 4, docs/0010
decision 6). Inference never reads one: if inference proves the
same type, the native path is taken *because of the proof*; if it proves
something else or proves nothing, the annotation is discarded, the value
stays `dynamic`, and you get a warning naming both. `function f(x: number)`
reached with a string compiles and runs as JavaScript. Annotations are read
in TypeScript's spellings (`string`, `boolean`, `number`, `any`, `unknown`,
…) as well as bronze's own IL names; text bronze cannot read at all is a
hard error naming the vocabulary, not a silent skip.

```
bronze il   <file> [--no-infer]
bronze build <file> -o <exe> [--no-infer]
```

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

## Layout

| Path | Contents |
|---|---|
| `src/support` | Source buffers, spans, diagnostics |
| `src/lex` | Hand-written lexer (TS core) |
| `src/ast` | AST nodes + visitor + canonical dump |
| `src/parse` | Recursive-descent parser, split by grammar seam: `parser_stmt` (cursor + statements), `parser_expr`, `parser_literal` (escapes, templates, object/array literals), `parser_func` (functions, arrows, classes) |
| `src/types` | Type/shape inference over the AST — lattice, flow analysis, shape classes, call-graph signatures, canonical dump. Produces a side table; mutates nothing (docs/0010) |
| `src/lower` | AST + inference side table → IL. Split by seam: `lower_infer` (what may be believed), `lower_scope` (closures), `lower_control` (block-argument SSA), `lower_expr`, `lower_object`, `lower_stmt` |
| `src/il` | Typed SSA IL: types, module model, canonical printer, verifier |
| `src/codegen` | Backend interface |
| `src/codegen-llvm` | LLVM backend (gated: `BRONZE_WITH_LLVM`): `llvm_abi` (helper declarations from the ABI registry), `llvm_prop` (inline property caches), `llvm_func`/`llvm_ops`/`llvm_arith` (one IL function's body), `llvm_backend` (module in, object file out) |
| `src/abi` | The generated-code ABI (`bronze_abi.h`) and its pure-C compile check — the only place a runtime helper signature is written |
| `src/runtime` | The dynamic value model: NaN-boxing, heap + GC, shapes, objects, arrays, strings, environments (docs/0004). The ABI helpers are `rt_state` (process-wide state and the caches rooted with it), `rt_convert`, `rt_object`, `rt_prop`, `rt_iter`, `rt_print`, `rt_members` (what ECMA-262 defines and bronze has not built) |
| `src/json` | The JSON grammar alone (RFC 8259 / ECMA-262 25.5.1): code units in, a tree out. Deliberately not `src/parse` — it exists for what it REFUSES that JavaScript accepts (docs/0022) |
| `src/rt` | The static library compiled output links against |
| `src/cli` | `bronze` driver (`lex`, `parse`, `types`, `il`, `build`, `version`) |
| `tests/<module>` | doctest suites, one per module |
| `tests/oracle` | Differential cases with pinned `.expected` stdout (docs/0003) |
| `docs/` | Numbered decision/plan docs + architecture |
