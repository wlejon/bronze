# 0001 — Foundation: goals, decisions, and phase plan

Status: active.

## Why bronze exists (and why broc was retired)

broc (D:\projects\broc) reached genuine milestones — a self-hosted
TS-implemented compiler whose native output was byte-identical to its node
output, all the way to a subset self-compilation fixpoint. But its
foundation contradicted the product goal: it used QuickJS `JSValue` as the
universal value representation, so every object/string/property operation
in compiled output paid dynamic-JS prices. Measured result: ~50x slower
than node on compiler workloads, with a memory model (refcount + pool
frames) that pinned ~5x node's live set. QuickJS was wanted for
*dynamically loaded code* (UI layer); it was never supposed to be the
representation of statically compiled TS.

bronze inverts the defaults:

- **Static representation wherever analysis proves it.** The input is wild
  JavaScript (three.js must compile — no strictness the ecosystem doesn't
  have). Shape/type INFERENCE produces struct layouts and typed IL ops; TS
  annotations, when present, are untrusted hints that seed inference.
  `dynamic` in the IL is the explicit fallback for what analysis cannot
  type — the fallback, not the substrate (broc's inversion).
- **Faster than node is a stated goal**, not a hoped-for side effect.
- **C++ implementation.** The TS implementation forced an interpreter/eval
  face, node heap ceilings, and a runaway-prone iteration loop.
- **LLVM backend**: world-class codegen for free rather than years of
  hand-rolled optimizer work. Own mid-level IL so the frontend never welds to
  LLVM.

## Decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | Name: `bronze` | bro family; casting metal → finished native form |
| 2 | Impl language: C++20 | User call; matches bro/brokit house stack |
| 3 | Build: CMake + vcpkg (pinned baseline), Ninja + clang-cl dev preset | Same as bro; fast iteration |
| 4 | Source language: JavaScript — wild, untyped, real-world (three.js must compile). TS annotations are untrusted optimization hints only | Product goal unchanged: web-stack apps shipped native; no strictness the ecosystem doesn't have |
| 5 | Own typed SSA IL with canonical text form | Carry broc's proven differential-ratchet discipline |
| 6 | LLVM as production backend, behind `BRONZE_WITH_LLVM` | Perf goal; heavy dep must never tax daily iteration |
| 7 | Parser: recursive descent; AST: visitor pattern | User preference; standard, debuggable |
| 8 | Hard errors over silent fallbacks, everywhere | broc lesson: every silent path eventually lied |
| 9 | Full input consumption enforced in every parser | broc's .form parser silently dropped modules 2..N |
| 10 | Deterministic output only: no locale, no map-order, `to_chars` floats | broc shipped a localeCompare bug and a map-order bug |

## What carries over from broc (methodology, not code)

- **Differential ratchets**: every stage has a canonical text dump
  (`ast::dump`, `il::print`) compared byte-for-byte in tests. Ratchet lists
  only grow.
- **Scoped tests per module** with one full proof before each commit.
- **Big-library isolation**: the failure mode where touching anything
  rebuilt QuickJS/duplicated 60MB MASM assemblies must not recur. LLVM is
  feature-gated in vcpkg, provisioned once, binary-cached.
- **Loud boundaries**: unimplemented paths are configure-time or runtime
  hard errors with instructions, never quiet no-ops.

## Phases

1. **Foundation (this doc, done)**: repo, build, module pattern, lexer,
   recursive-descent parser for the TS core, AST+visitor+dump, IL model +
   canonical printer, CLI (`lex`, `parse`), tests per module.
2. **LLVM provisioning + minimal end-to-end (done, docs/0002)**: vcpkg
   `llvm` feature build; `codegen-llvm` implements `Backend::emitObject` for
   the current IL op set; `bronze build main.ts` → exe printing a number.
   Perf smoke vs node from day one.
3. **Types (done, docs/0010)**: inference-first — shape and type analysis
   over untyped JS producing IL types/layouts, with `dynamic` as the
   proven-safe fallback. An annotation is compared against what inference
   proved and discarded with a warning when no proof backs it, which is what
   makes decision 4 above true rather than aspirational. `src/types` is the
   module; `--no-infer` is the bisection seam and the oracle suite runs every
   case both ways.
4. **Language growth**: builtins and prototype methods (docs/0011), the
   ES2015 syntax three.js is written in (docs/0012), container printing
   (docs/0013), automatic semicolon insertion (docs/0014), operators
   (docs/0015), names and literals (docs/0016), binding patterns
   (docs/0017), selection and jumps (docs/0018), `delete` and accessors
   (docs/0019), exceptions (docs/0020), iteration and collections
   (docs/0021), exact numbers and JSON (docs/0022), and ES modules
   (docs/0023) — a build is a graph of files rather than one file. Still
   open at the end of the phase: the temporal dead zone (and with it module
   cycles), the intrinsic prototype objects, generators, and bare module
   specifiers.
5. **Dynamic boundary**: the `dynamic` type's runtime and the QuickJS
   interop seam for hot-loaded UI code — as a boundary module, never as the
   default representation.

## Provisioning LLVM (phase 2, one-time)

```
D:\vcpkg\vcpkg install --triplet x64-windows --x-feature=llvm
```

Expect hours of build time and significant disk; it is cached afterwards.
Then configure with `-DBRONZE_WITH_LLVM=ON`. Never make the default build
depend on it.
