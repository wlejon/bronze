# bronze

[![CI](https://github.com/wlejon/bronze/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/bronze/actions/workflows/ci.yml)
[![Nightly](https://github.com/wlejon/bronze/actions/workflows/nightly.yml/badge.svg)](https://github.com/wlejon/bronze/releases/tag/nightly)
[![CodeQL](https://github.com/wlejon/bronze/actions/workflows/codeql.yml/badge.svg)](https://github.com/wlejon/bronze/actions/workflows/codeql.yml)

An ahead-of-time compiler that turns JavaScript into a native executable.

```
bronze build app.js -o app
./app
```

bronze compiles JavaScript to machine code through LLVM. It follows `import`s 
from your entry file, compiles the whole program, and links a standalone binary.
The test suite compiles three.js r160 and pixi.js v8 from unmodified source.

## What you can build

**A standalone executable.** `bronze build app.js -o app` produces a single
binary with bronze's runtime linked in.

**An app embedded in a larger program.** `--emit-obj` produces an object
file for your own build to link, and `--host-globals` lists the globals your
host provides (`document`, `requestAnimationFrame`, and so on).
`--emit-shared` produces a module your host opens at run time instead, linked
against a shared bronze runtime so one process can load several. The bro
engine uses this to run compiled three.js apps against its DOM and WebGL.
The app's scene graph and render loop run as machine code while the engine
supplies the browser surface. `src/embed` is the C++ API a host uses to
drive the compiled program, register globals, and hold GC-safe handles.

## How it works

bronze infers types by analysing the whole program: each binding's type at
each point, the shape of each object site, each function's signature across
its call sites. Where inference proves a type, the compiler emits direct
native code; everywhere else it uses boxed dynamic values with inline
caches, which keeps every program correct while the proven parts run fast.
`bronze types app.js` prints what inference concluded about your code.

TypeScript annotations are accepted and treated as hints. When inference
confirms an annotation, the code takes the native path; when it can't, the
annotation is dropped with a warning and the code runs with its actual
JavaScript behaviour.

The performance target is concrete: compiled output should beat nodejs on the
same program, and benchmarks that fall short are treated as bugs.

## Status

bronze implements enough of ECMA-262 to compile large real-world libraries
(three.js and pixi.js are the pinned milestones). Programs are compiled
ahead of time, so `eval` and `new Function` are out of scope. When a program
uses a language feature bronze hasn't implemented yet, compilation stops
with an error naming the feature.

## The CLI

```
bronze build <entry.js> -o <exe>          compile and link an executable
bronze build <entry.js> -o <obj> \
             --emit-obj \
             --host-globals <manifest>    compile to an object for a host build
bronze build <entry.js> -o <lib> \
             --emit-shared                compile a module a host loads at run time
bronze types <entry.js>                   show what inference proved
bronze il    <entry.js>                   dump the typed IL
bronze lex / bronze parse                 earlier pipeline stages
bronze version                            print the version
```

`<entry.js>` is the entry of a module graph: `import`/`export` with relative
specifiers pull in the rest. `--no-infer` compiles everything on the dynamic
path; it exists as a debugging tool for isolating inference bugs
([docs/internals.md](docs/internals.md) has the details).

## Getting bronze

[Nightly builds](https://github.com/wlejon/bronze/releases/tag/nightly) are
published for Windows x64, Linux x64, and macOS arm64 — each zip is the
compiler plus the runtime libraries it links into compiled programs, tested
against the full suite before publishing. Unzip anywhere and run
`bronze build app.js -o app`. You need a system linker on your machine
(Windows: MSVC's `link.exe`; Linux/macOS: `clang++` or `g++`); bronze does
not ship one.

## Building bronze

Prereqs: CMake ≥ 3.24, Ninja, a C++20 compiler, and vcpkg. On Windows,
`dev.cmd` wraps the MSVC environment: `.\dev.cmd cmake ...`.

```
cmake --preset dev -DBRONZE_WITH_LLVM=ON
cmake --build --preset dev
ctest --preset dev
```

`-DBRONZE_WITH_LLVM=ON` enables the backend that emits objects and
executables. vcpkg builds LLVM from source the first time (hours,
binary-cached afterwards). Configuring without the flag builds the compiler
front half (lexer, parser, inference, IL) with a fast test loop for
frontend work. [docs/internals.md](docs/internals.md) has the full
build-and-iteration workflow.

## Testing

Correctness rests on a differential suite
([tests/oracle/README.md](tests/oracle/README.md)): JavaScript programs
whose exact stdout is pinned in committed files derived from ECMA-262. Every
case compiles and runs twice (with inference and with `--no-infer`) and
both runs must produce identical bytes, so the analysis and the dynamic
fallback check each other. Compiled objects carry an ABI fingerprint the
runtime verifies at startup, so an object built against an older runtime
fails at launch with both versions named.

## More

- [docs/internals.md](docs/internals.md) the pipeline, repository layout,
  inference and `--no-infer` in depth, embedding details, and the iteration
  workflow.
- [tests/oracle/README.md](tests/oracle/README.md) how the differential
  suite works and how to add a case.

## License

[MIT](LICENSE)
