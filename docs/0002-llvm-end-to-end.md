# 0002 — LLVM backend: first end-to-end executable

Status: next up (blocked only on the vcpkg `llvm` feature build finishing).
Goal: `bronze build main.js -o main.exe` produces a running native exe, with
a perf smoke against node from the first day.

## Steps

1. **Wire the dependency.** `find_package(LLVM CONFIG REQUIRED)` inside
   `src/codegen-llvm` only (replace the FATAL_ERROR gate). Link the minimal
   component set (`core`, `x86codegen`, `passes`); print the resolved LLVM
   version at configure. The default build must remain untouched — verify
   by configuring without `-DBRONZE_WITH_LLVM=ON` and confirming zero LLVM
   references in the build graph.
2. **`Backend::emitObject` for the current IL op set** (consts, arithmetic,
   compares, call, ret over f64/i32/bool). One IL function → one LLVM
   function; direct mapping, no optimization creativity — LLVM's default
   `-O2` pipeline does the work. Verifier on (`verifyModule`) before
   emission; a verifier failure is a hard error carrying the IL text dump.
3. **Lowering: AST → IL** for the same subset (`src/lower`, new module).
   Number-typed code only in this phase; anything outside the subset is a
   diagnosed hard error naming the construct ("not yet lowerable"), never a
   silent skip. This module is where inference will plug in later; for now
   every value is f64 (JS number semantics) — correct first, typed later.
4. **Link step.** Invoke `lld-link` (ships with the LLVM toolchain already
   in Program Files) against the object file; entry calls `main` and prints
   its f64 result via a 20-line C runtime stub (`src/rt`, compiled once).
   Printing must match node's number formatting (shortest round-trip — we
   already have `to_chars`) because the oracle harness (doc 0003) compares
   stdout bytes.
5. **CLI: `bronze build <file> -o <exe>`** wiring the whole pipeline, and
   `bronze il <file>` printing the canonical IL (the debugging seam).
6. **Perf smoke, day one.** `bench/` with two microbenchmarks (iterative
   fib, numeric loop) run by a script that times bronze-built exe vs
   `node`. Numbers recorded in this doc's Log section every time the
   backend changes. No target yet — the point is a trend line from day one,
   so a broc-style "50x surprise" is impossible.

## Exit criteria

- `bronze build bench/fib.js -o fib.exe && ./fib.exe` prints the same bytes
  as `node bench/fib.js`.
- All module suites green; codegen-llvm has its own suite (IL → exe → run →
  compare, per fixture).
- Perf log has its first entries. Expected: bronze ≥ node on these numeric
  microbenches (no dynamic ops exist yet to hide behind — if we're slower
  here, something is wrong and gets fixed now, not later).

## Log

(append: date, benchmark, bronze time, node time, notes)
