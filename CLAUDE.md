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
  make something pass. Oracle cases (docs/0003) compare stdout to a
  committed `.expected` file byte-for-byte — never edit an expectation
  to match bronze's output, and never normalize bytes to force a match.
- **node is not a dependency.** Tests, builds, and benchmarks must never
  invoke node; expectations are pinned files (docs/0003).
- **Module isolation**: each `src/<module>` is a static lib + own doctest
  binary + ctest label; dependencies flow through `bronze::<module>`
  targets only; the CLI is the composition root.
- **No source file over 1000 lines.** Split along a seam that names
  something — a grammar production, an instruction family, a receiver kind —
  never at an arbitrary line count.
- **Comments say why, about the code in front of them.** Not what the code
  used to be, not when it changed; git carries that. Same for the docs: they
  record decisions and their rationale, never a changelog.
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
- 0003 differential harness — pinned .expected files (node-free since 2026-08-10)
- 0004 dynamic value model (NaN-boxing / shapes / GC / strings / arrays — accepted 2026-08-10)
- 0005 control flow (blocks / block-argument SSA / truthiness — designed 2026-08-10)
- 0006 rooting generated code (GC root frames in compiled output — completes 0004 decision 3)
- 0007 closures (environment records per scope, threaded through the calling convention)
- 0008 prototypes (`this` / `new` / proto chain / depth-caching proto ICs)
- 0009 enumeration order (spec'd own-key order via the shape chain; dictionary boundary)
- 0010 inference (phase 3: lattice, shape classes, call-graph signatures,
  annotations as untrusted hints, inline property caches — `src/types`)
- 0011 builtins (provided globals by name, `Math`, `Array.prototype`,
  `String.prototype`; an unbuilt member stays a named error)
- 0012 syntax growth (string-escape decoding, template literals, for-of
  as an index walk, arrows and lexical `this`, classes desugared)
- 0013 printing containers (console.log's inspect format, and the
  divergences from node that are deliberate)
- 0014 automatic semicolon insertion (the restricted productions, and the
  unreachable-code and void-call bugs it uncovered)
- 0015 operators (bitwise and shifts as int32 inside / number outside,
  `**`, `typeof`, `instanceof`, `in`, `void`, comma, loose equality, and
  the precedence ladder that fixed assignment)
- 0016 names, declarations and literals (the module scope as a singleton
  record, the binding leak between `lower()`'s functions, binding lists,
  numeric literal forms, computed keys, multi-argument `console.log`)
- 0017 binding patterns (parameter defaults, rest, spread, destructuring in
  declarations / parameters / assignments / for-of heads, and the derived
  class's implicit forwarding constructor)
- 0018 selection, jumps and chains (`for-in` as a key snapshot, enumerability
  on the shape, `switch` as a test chain plus a body chain, one jump stack for
  `break`/`continue`/labels, and the optional chain as an n-way join)
- 0019 delete and accessors (dictionary mode as the escape from the shape
  chain, array holes and which methods skip them, the receiver an accessor
  runs with, and what an inline-cache entry may describe)
- 0020 exceptions (the pending-exception cell and why not landingpads, the
  handler as a property of the block, `finally` by duplication per exit path,
  which runtime fatals became catchable, and the `Error` family)
- 0021 iteration and collections (`Symbol.iterator` as a well-known string
  key, the iteration record with fast kinds beside the protocol, IteratorClose
  as a cleanup frame, Map/Set as a table whose index carries a GC epoch, and
  `writable`/`configurable`/`extensible` in the dictionary)
- 0022 exact numbers, JSON, and the rest of `Object` (object-literal method
  shorthand, `toFixed` from bignum arithmetic on the double itself, JSON as its
  own grammar module, `setPrototypeOf` made safe by dictionary mode, and the
  Map epoch tied to the copy that relocates)
- 0023 ES modules (the graph as one flat namespace so an import binding IS the
  exporting module's binding, relative specifiers only, cycles refused by
  name, the namespace object as a literal of getters, and multi-file oracle
  cases as directories)
- 0024 regular expressions (the `/` ambiguity decided from the previous token,
  a pattern grammar and backtracking matcher in `src/regex` compiled at the
  literal, patterns shared by source through an index not a pointer, case
  folding refused above Latin-1 by name, and the match array as an array with
  named properties)
- 0025 `new` on any callee (the callee as an expression and the four groupings
  that fall out of stopping it at the argument list, `new Foo` without one,
  the shape class a bare name still buys, the renamer recursing into it, and
  the `Foo.prototype.constructor` back-pointer `new this.constructor()` needs)
- 0026 generators and console streams (the straight-line subset desugared in
  the parser into an iterator over a step index, every construct outside it
  refused by its own name, `[Symbol.iterator]` matched syntactically, the
  missing receiver of `o[k]()`, the receiver slot an arrow must not own, and
  `warn`/`error` on stderr so the oracle's pinned stdout never protects
  library chatter)
- 0027 unresolvable names and `arguments` (the provable/unprovable line that
  makes a free name a runtime `ReferenceError` plus a warning while
  `console.table` stays a compile error, bare `typeof` exempt and unwarned,
  the named function expression kept out of it, `arguments` as an ordinary
  binding carried in by the call wrapper with padding turned off, and the
  four global numeric functions)
