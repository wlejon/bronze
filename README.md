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

## Layout

| Path | Contents |
|---|---|
| `src/support` | Source buffers, spans, diagnostics |
| `src/lex` | Hand-written lexer (TS core) |
| `src/ast` | AST nodes + visitor + canonical dump |
| `src/parse` | Recursive-descent parser |
| `src/il` | Typed SSA IL: types, module model, canonical printer |
| `src/codegen` | Backend interface |
| `src/codegen-llvm` | LLVM backend (gated: `BRONZE_WITH_LLVM`) |
| `src/cli` | `bronze` driver (`lex`, `parse`, `version`) |
| `tests/<module>` | doctest suites, one per module |
| `docs/` | Numbered decision/plan docs + architecture |
