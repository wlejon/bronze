# bronze — project instructions

AOT compiler for JavaScript (wild, untyped JS — three.js is the bar) in
C++20. Native layouts wherever inference proves them; `dynamic` is the
fallback, never the substrate. Goal: faster than node for typed/inferable
code. Read `docs/0001-foundation.md` first; `docs/0000-broc-postmortem.md`
explains every reference to "broc" (the retired TypeScript predecessor at
`D:\projects\broc` — read-only, methodology donor).

## Build & test (Ninja + cl via the vcvars wrapper)

```
.\dev.cmd cmake --preset dev            # configure
.\dev.cmd cmake --build --preset dev    # build (incremental ~2s)
.\dev.cmd ctest --preset dev            # all tests
.\dev.cmd ctest --preset dev -L lex     # one module's tests
```

Iterate with scoped module tests; run the full `ctest` before any commit.

## Hard rules

- **Never make the default build depend on heavy libraries.** LLVM lives
  behind the vcpkg `llvm` feature + `-DBRONZE_WITH_LLVM=ON` only.
- **Hard errors over silent fallbacks.** Unimplemented constructs are
  diagnosed by name; no quiet skips, no placeholder output.
- **Every parser consumes all input or errors.** No silent drops.
- **Deterministic output only**: no locale functions, no hash-map
  iteration order in output paths, floats via `std::to_chars`.
- **Ratchets only grow**: never weaken or remove a pinned test/case to
  make something pass. Oracle cases (docs/0003) compare stdout to node
  byte-for-byte — never normalize bytes to force a match.
- **Module isolation**: each `src/<module>` is a static lib + own doctest
  binary + ctest label; dependencies flow through `bronze::<module>`
  targets only; the CLI is the composition root.
- Recursive descent for parsers, visitor for AST traversal (house
  preference).
- **The generated-code ABI lives in `src/abi/bronze_abi.h`, and only
  there.** Pure C, primitives only (u64 in / u64 out); every helper
  generated code calls is an X(...) line in its registry, which expands
  into both the C prototypes and codegen-llvm's LLVM declarations. Never
  hand-declare a runtime symbol in the backend, and never put a C++ type
  in a signature generated code touches — MSVC returns classes via hidden
  sret, which silently shifts every argument register (the 2026-08-10
  dynamic-call crash).

## Docs index

- 0000 broc post-mortem (why bronze exists; the lessons as rules)
- 0001 foundation (decisions, phases)
- 0002 LLVM end-to-end plan (current work)
- 0003 node-as-oracle differential harness
- 0004 dynamic value model (NaN-boxing / shapes / GC / strings / arrays — accepted 2026-08-10)
