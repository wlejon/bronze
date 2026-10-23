# bronze

[![CI](https://github.com/wlejon/bronze/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/bronze/actions/workflows/ci.yml)
[![Nightly](https://github.com/wlejon/bronze/actions/workflows/nightly.yml/badge.svg)](https://github.com/wlejon/bronze/releases/tag/nightly)
[![CodeQL](https://github.com/wlejon/bronze/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/bronze/actions/workflows/codeql.yml)

An AOT/JIT compiler, single-source GPU shader compiler, and formal verification & superoptimization engine for JavaScript, built on [Brass](https://github.com/wlejon/brass).

```bash
bronze build app.js -o app
./app
```

bronze follows `import`s from your entry file, infers types and proofs across the whole program, and writes a standalone native executable — no external linker or toolchain required. The test suite compiles unmodified `three.js` r160 and `pixi.js` v8 from source.

## What bronze does

- **Ahead-of-time & in-memory JIT compilation.** `bronze build app.js -o app` emits a native executable, its compiled module (`.dll` / `.so` / `.dylib`), and bronze's runtime. `bronze run app.js` and `bronze eval "expr"` compile and execute directly in memory via the Brass JIT.
- **Embeddable native modules.** `--emit-obj` and `--emit-shared` compile JS against a `--host-globals` manifest so host engines (like [bro](https://github.com/wlejon/bro)) run compiled apps as native machine code against their own DOM, WebGL, physics, and audio APIs.
- **Proof-driven native codegen.** Whole-program type, shape, interval, and nullish inference works together with Brass's in-process bitvector SMT solver (`BVSolver`) to prove bounds, integrality, and value tags — stripping NaN-boxing, bounds checks, and type guards from hot loops to unlock unboxed `i32`/`f64` and `AVX2`/`NEON` SIMD codegen.
- **Single-source JS-to-GPU compilation (`@kernel` / `@shader`).** Annotated JS functions — or plain `TypedArray` loops promoted via `bronze auto-kernel` — compile directly to Vulkan **SPIR-V** and CUDA **PTX** (compute, vertex, and fragment stages), with symbolic reverse-mode automatic differentiation (`js_gpu_autodiff`) and static proofs of memory safety, barrier uniformity, and CPU-vs-GPU equivalence.
- **Formal verification, constraint solving & CEGIS superoptimization.** Because the compiler and SMT solver share one IR in-process, bronze can run JS functions backwards (`solveFunction`), prove two functions mathematically equivalent (`prove-equiv`), prove cross-platform float determinism (`prove-determinism`), synthesize minimal edge-case test vectors (`synth-tests`), and rewrite JS source into shorter, faster, mathematically proven equivalents (`bronze optimize`).

## The CLI

```
bronze run               <entry.js>                run JS in-memory via JIT
bronze eval              <code> (or -e <code>)     evaluate JS expression in-memory
bronze build             <entry.js> -o <exe>       compile to a standalone native program
bronze build             <entry.js> -o <obj> \
                         --emit-obj \
                         --host-globals <manifest> compile to a relocatable object for a host build
bronze build             <entry.js> -o <lib> \
                         --emit-shared             compile a shared module a host loads at runtime
bronze check             <entry.js>                static type, bounds, nullish & safety verification
bronze optimize          <entry.js> [--write] \
                         [--diff] [--superopt]     verified JS source-to-source optimizer & CEGIS rewriter
bronze auto-kernel       <entry.js> [--write]      rewrite proven-parallel TypedArray loops into GPU kernels
bronze prove-equiv       <file.js> <fnA> <fnB>     prove two JS functions bit-identical via SMT
bronze prove-determinism <entry.js>                prove cross-platform IEEE-754 / netcode determinism
bronze synth-tests       <entry.js>                synthesize minimal branch-covering unit tests via SMT
bronze types             <entry.js>                show inferred types and proof certificates
bronze il                <entry.js>                dump the typed IL
bronze lex / parse       <entry.js>                inspect lexer / parser output
bronze version                                     print the version
```

`<entry.js>` is the root of a module graph (`import`/`export` pull in dependencies). `--no-infer` forces the boxed dynamic path for differential debugging.

## How it works

bronze infers types and value invariants across the whole module graph: each binding's type and interval at each program point, the shape of each object site, and each function's signature across its call sites. TypeScript annotations are accepted as hints and verified by inference.

Where inference and `StaticChecker` prove a type, integer range, or array bound, the compiler emits direct unboxed machine instructions and feeds proof certificates into Brass's MIR/LIR superoptimizer and SIMD vectorizer. Everywhere else it uses NaN-boxed dynamic values with inline caches and deoptimization guards, keeping all ECMA-262 behavior exact while proven paths run at native C++/SIMD speed. Dynamic constructs like `eval` and `new Function` execute in-process via the Brass JIT (`src/eval`).

## Getting & building bronze

[Nightly builds](https://github.com/wlejon/bronze/releases/tag/nightly) are published for Windows x64, Linux x64, and macOS arm64. Unzip anywhere and run `bronze build app.js -o app`.

To build from source (requires CMake ≥ 3.24, Ninja, a C++20 compiler, and vcpkg; on Windows use `.\dev.cmd cmake ...`):

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Brass (`../brass`) is fetched automatically at configure time if not checked out alongside `bronze`.

## Testing

Correctness is enforced by:
1. **Differential ECMA-262 oracle suite** ([tests/oracle/README.md](tests/oracle/README.md)): every program compiles and runs twice (with inference and with `--no-infer`) and must produce byte-identical stdout.
2. **Formal discovery & equivalence suites** (`tests/check/`, `tests/discovery/`): SMT-backed equivalence proofs over 3D math, codecs, hashes, physics integrators, GPU shaders, and standard library routines.

## Documentation

- [docs/internals.md](docs/internals.md) — pipeline, repository layout, inference, embedding, and iteration workflow
- [docs/dynamic-eval.md](docs/dynamic-eval.md) — in-memory dynamic evaluation and JIT execution (`src/eval`)
- [docs/formal_discovery_report.md](docs/formal_discovery_report.md) — bugs and arithmetic invariants discovered across JS libraries via `BVSolver`
- [docs/pin-census.md](docs/pin-census.md) · [docs/shape-census.md](docs/shape-census.md) · [docs/slot-representation.md](docs/slot-representation.md) · [docs/env-promotion.md](docs/env-promotion.md)
- [tests/oracle/README.md](tests/oracle/README.md) — differential oracle suite guide

## License

[MIT](LICENSE)
