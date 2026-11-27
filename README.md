# bronze

[![CI](https://github.com/wlejon/bronze/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/bronze/actions/workflows/ci.yml)
[![Nightly](https://github.com/wlejon/bronze/actions/workflows/nightly.yml/badge.svg)](https://github.com/wlejon/bronze/releases/tag/nightly)
[![CodeQL](https://github.com/wlejon/bronze/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/bronze/actions/workflows/codeql.yml)

An ahead-of-time compiler that turns JavaScript into a native executable.

```
bronze build app.js -o app
./app
```

bronze compiles JavaScript to machine code through Brass. It follows `import`s
from your entry file, compiles the whole program, and writes a native program —
no linker, no toolchain, nothing on the machine but bronze itself.
The test suite compiles three.js r160 and pixi.js v8 from unmodified source.

## What you can build

**A native program.** `bronze build app.js -o app` writes three files in one
directory: `app`, the compiled program as a loadable module beside it (`app.dll`
/ `app.so` / `app.dylib`), and bronze's shared runtime. The executable is a
copy of bronze's program host, which opens the module named after itself; the
three travel together.

**Direct in-memory execution / JIT.** `bronze run app.js` compiles and runs
scripts and module graphs directly in memory with zero disk I/O, and
`bronze eval "2 + 2"` (or `-e`) evaluates expressions on the fly.

**An app embedded in a larger program.** `--emit-obj` produces an object
file for your own build to link, and `--host-globals` lists the globals your
host provides (`document`, `requestAnimationFrame`, and so on).
`--emit-shared` produces a module your host opens at run time instead, linked
against a shared bronze runtime so one process can load several. `src/embed`
is the C++ API a host uses to drive the compiled program, register globals,
and hold GC-safe handles (`Persistent`, `HandleScope`, `Local<T>`). A host can
also compile a script ahead of time through the embedded evaluator
(`eval::buildModule`): the same program the JIT would run, written as a
loadable module with a record of what it was built from, which
`eval::loadModule` checks before running it, so a stale module falls back to
the JIT instead of running. The bro engine compiles its apps this way, in
process: an installed app's scripts are built on the harvest of its first run
(`EvalOptions::harvestOutPath` / `feedbackInPath`), and its scene graph and
render loop run as machine code while the engine supplies the browser
surface.

## How it works

bronze compiles JavaScript cleanly through one harvest-guided speculative AOT
pipeline (`--harvest-out` followed by `--feedback-in`) and one Auto multi-tier
Brass JIT (`src/eval`):

- **Speculative AOT (`$spec` / `$opt` with generic `G` fallback):** whole-program
  type/shape analysis (`bronze types app.js`) pairs with runtime inline-cache
  harvests to emit direct native loops, unboxed numeric arithmetic, guarded
  slot accesses, and inlined methods/constructors. Any guard miss exits cleanly
  to the generic `G` twin via resume points without corrupting state.
- **In-memory JIT (`bronze run` / `bronze eval` / host `eval`):** runs on the
  Auto multi-tier Brass pipeline (Fast Tier-0 interpreter with background
  Tier-1, Tier-2, and OSR), with compartment-isolated abort handling so host
  runtimes stay protected.
- **Shared memory & atomics:** full process-wide `SharedArrayBuffer` backing
  and `Atomics.wait`/`notify`/`waitAsync` support across worker threads.

TypeScript annotations are accepted and treated as hints. `.ts`, `.mts` and `.cts` files are read as TypeScript: types, generics,
assertions and overloads are erased, and enums, namespaces and parameter
properties become the JavaScript node's TypeScript transform emits for them.
A `.js` file is read as JavaScript, so tokens TypeScript would read
differently (`a < b > (c)`, `type`, `namespace`) mean what JavaScript says;
plain annotations (`let x: number`, `function f(a: T): R`) are still
accepted there.

The performance target is concrete: harvest-guided compiled output beats Node.js on the
same program, and benchmarks that fall short are treated as bugs.

## Status

bronze passes 1,099 ECMA-262 differential oracle cases across `run`, `aot`,
`profile`, `switch`, and `adversarial` modes (0 blocked cases remain) and compiles
unmodified three.js r160 and pixi.js v8 from source. Ahead-of-time compilation
turns the full module graph into native machine code. Dynamic constructs
like `eval` and `new Function` (including generator and async functions)
are evaluated dynamically in-memory via the Brass JIT engine (`src/eval`).
When a program uses a language feature bronze hasn't implemented yet,
compilation stops with an error naming the feature.

## Verification

`src/check` provides static verification and `int32` bitvector constraint
solving over straight-line JavaScript functions (backed by Brass's `BVSolver`):

- `bronze check <entry.js>`: abstract-interpretation and bitvector hazard checks
  (null/undefined dereference, division by zero, typed-array bounds, and,
  with `--deterministic`, a replay audit: wall clock, unseeded random,
  `for..in` order, outer state).
- `bronze prove-equiv <a.js> <b.js> --fn <name>`: bitvector equivalence check
  for straight-line integer JS functions, reporting a concrete counterexample
  when outputs differ.

## The CLI

```
bronze run   <entry.js>                   run JS directly in-memory via JIT
bronze eval  <code> (or -e <code>)        evaluate JS code directly in-memory via JIT
bronze build <entry.js> -o <exe>          compile to a native program (the
                                          executable, its module, its runtime)
bronze build <entry.js> -o <obj> \
             --emit-obj \
             --host-globals <manifest>    compile to an object for a host build
bronze build <entry.js> -o <lib> \
             --emit-shared                compile a module a host loads at run time
bronze check <entry.js>                   run static & bitvector hazard checks
bronze types <entry.js>                   show what inference proved
bronze il    <entry.js>                   dump the typed IL
bronze lex / bronze parse                 earlier pipeline stages
bronze version                            print the version
```

`<entry.js>` is the entry of a module graph: `import`/`export` with relative
specifiers pull in the rest. AOT builds pair whole-program inference with
inline-cache harvesting (`--harvest-out` followed by `--feedback-in`) for the
fastest native code, and `bronze run` / `bronze eval` execute on the Auto
multi-tier Brass JIT pipeline (Fast Tier-0 interpreter with background Tier-1,
Tier-2, and OSR).

## Getting bronze

[Nightly builds](https://github.com/wlejon/bronze/releases/tag/nightly) are
published for Windows x64, Linux x64, and macOS arm64 — each zip is the
compiler, the shared runtime and the program host it copies beside every
program it builds, tested against the full suite before publishing. Unzip
anywhere and run `bronze build app.js -o app`. Nothing else is needed: bronze
writes its own executables and modules, so there is no linker to install.

## Building bronze

Prereqs: CMake ≥ 3.24, Ninja, a C++20 compiler, and vcpkg. On Windows,
`dev.cmd` wraps the MSVC environment: `.\dev.cmd cmake ...`.

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The native code generation backend is Brass (`src/codegen-brass`), which compiles
Bronze IL to native machine code (for both AOT builds and the multi-tier JIT).
Brass is a dependency, not a submodule: a
working tree at `../brass` is used when there is one, otherwise configure fetches
the head of brass's main (`cmake/bro_deps.cmake`; a release tag's
`cmake/bro_lock.cmake` fixes the commit).

## Testing

Correctness rests on a differential suite
([tests/oracle/run.js](tests/oracle/run.js), compiled to `bronze-oracle` by
`bronze build` itself): JavaScript programs
whose exact stdout is pinned in committed files derived from ECMA-262. Every
case runs through `run`, `aot`, `profile`, `switch`, and `adversarial` modes
(plus the `three.js` and `pixi.js` milestones), and all must produce identical
bytes. Compiled objects carry an ABI fingerprint the
runtime verifies at startup, so an object built against an older runtime
fails at launch with both versions named.

## License

[MIT](LICENSE)
