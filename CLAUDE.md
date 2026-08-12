# bronze — project instructions

AOT compiler for JavaScript (wild, untyped JS — three.js is the bar) in
C++20. Native layouts wherever inference proves them; `dynamic` is the
fallback, never the substrate. Goal: faster than node for typed/inferable
code. Read `README.md` first. "broc" is the retired TypeScript predecessor at
`D:\projects\broc` — read-only, methodology donor. It landed ~50x slower than
node because it used QuickJS values as the universal representation, which is
the mistake bronze's design is a reaction to.

## Build & test (Ninja + cl via the vcvars wrapper)

```
.\dev.cmd cmake --preset dev              # configure
.\dev.cmd cmake --build --preset dev      # build (incremental ~2s)
.\dev.cmd ctest --preset dev              # all tests (~7 min)
.\dev.cmd ctest --preset dev -L lex       # one module's tests
.\dev.cmd ctest --preset dev -LE threejs  # everything but the milestone (~4.5 min)
.\dev.cmd ctest --preset dev -L threejs   # the milestone alone (~2.5 min)
```

Iterate with scoped module tests; run the full `ctest` before any commit.

`oracle-threejs` compiles unmodified three.js r160 from vendored source and
checks the scene graph it builds (`tests/oracle/threejs/README.md`).
It is ~145 s of the run because the 28-file graph is compiled once per inference
mode. **It stays in the pre-commit run** — it is the only test that proves the
project's stated bar — but `-LE threejs` is the loop to iterate against.

## Hard rules

- **Never make the default build depend on heavy libraries.** LLVM lives
  behind the vcpkg `llvm` feature + `-DBRONZE_WITH_LLVM=ON` only.
- **Hard errors over silent fallbacks.** Unimplemented constructs are
  diagnosed by name; no quiet skips, no placeholder output.
- **Every parser consumes all input or errors.** No silent drops.
- **Deterministic output only**: no locale functions, no hash-map
  iteration order in output paths, floats via `std::to_chars`.
- **Ratchets only grow**: never weaken or remove a pinned test/case to
  make something pass. Oracle cases compare stdout to a
  committed `.expected` file byte-for-byte — never edit an expectation
  to match bronze's output, and never normalize bytes to force a match.
- **node is not a dependency.** Tests, builds, and benchmarks must never
  invoke node; expectations are pinned files (`tests/oracle/README.md`).
- **Module isolation**: each `src/<module>` is a static lib + own doctest
  binary + ctest label; dependencies flow through `bronze::<module>`
  targets only; the CLI is the composition root.
- **No source file over 1000 lines.** Split along a seam that names
  something — a grammar production, an instruction family, a receiver kind —
  never at an arbitrary line count.
- **Comments say why, about the code in front of them.** Not what the code
  used to be, not when it changed; git carries that. Prose that belongs to
  the code lives next to the code, not in a parallel document.
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
