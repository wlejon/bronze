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

- 2026-08-10: fib.js | bronze: 7.36ms | node: 31.03ms | 4.22x speedup (stdout byte match verified)
- 2026-08-10: numeric_loop.js | bronze: 7.44ms | node: 30.00ms | 4.03x speedup (stdout byte match verified)
- 2026-08-10 (dynamic substrate landed): fib.js | bronze: 9.52ms | node: 34.64ms | 3.64x (byte match)
- 2026-08-10 (dynamic substrate landed): numeric_loop.js | bronze: 9.41ms | node: 33.70ms | 3.58x (byte match)
- 2026-08-10: property_access.js | bronze: 894.36ms | node: 34.03ms | **0.04x — 26x slower** (byte match). First dynamic-path bench: every property access and dynamic call crosses into C++ runtime helpers with no inline-cache fast path in generated code. This is the baseline gap that inference (phase 3) and IL-level IC fast paths must close — recorded on day one so no broc-style surprise is possible.
- 2026-08-10 (control flow landed; benchmarks rewritten as real loops): all
  earlier entries are superseded — the old benches were hand-unrolled
  straight-line code that LLVM could constant-fold, so those numbers mostly
  measured process startup. New honest baseline (bronze-only; node is no
  longer invoked by any tooling, see docs/0003):
  - fib.js — recursive fib(30) — avg 112.0ms / min 105.7ms (output 832040, correct)
  - numeric_loop.js — 10M-iteration float loop — avg 86.9ms / min 81.8ms
  - property_access.js — 1M-iteration `o.a + o.b` loop — avg 890.1ms / min
    876.0ms. The dynamic-dispatch gap is now cleanly visible in one number:
    ~0.9µs per iteration of two prop.get helper calls + boxing. This is the
    number inference (a future doc) and generated-code IC fast paths must
    attack.
- 2026-08-10 (out-of-line slots landed, property keys interned): fib
  118.4/113.4ms, numeric_loop 88.1/82.8ms (both noise-level vs above);
  property_access.js — avg 144.5ms / min 138.1ms — **6.2x faster**. The
  890ms number was mostly self-inflicted allocation: every prop access
  built a fresh key string (≈48MB of garbage over the run — enough to
  trigger mid-run collection, which generated code cannot survive; see
  0004). Keys are now interned into the immortal arena once at
  registration, and the IC-hit path touches no key, no std::string, and no
  root registration. Remaining ~0.14µs/iteration is the helper-call +
  boxing overhead that inference and generated-code IC fast paths attack
  next.
- 2026-08-10 (generated code rooted, docs/0006): fib 118.3/113.0ms,
  numeric_loop 87.4/81.9ms, property_access 148.4/143.1ms. Cost of making
  compiled code survive a moving collection: **nothing** on the two f64
  benchmarks — they hold no `dynamic` values, so they get no root frame at
  all — and **~3%** on property_access. The first implementation registered
  each frame with a pair of helper calls and cost 2.1x on fib (248ms):
  a tiny, hot, all-`dynamic` function pays a fixed per-call cost twice.
  Linking the frame inline instead put fib back exactly where it was. The
  512MB heap reservation that existed only to postpone the first collection
  is now 64MB, so ordinary runs actually collect.
- 2026-08-11 (inference landed, docs/0010 steps 1-3 + 3b): fib 10.7/6.6ms
  (**11.3x** faster than the 118.3/113.0ms above), numeric_loop 37.7/33.4ms
  (**2.4x**), property_access 134.0/126.9ms (1.13x). Outputs unchanged and
  byte-exact.

  The two numeric benchmarks are the whole argument of docs/0001 decision 4
  in one line. Nothing about codegen changed: what changed is that
  inference proves `fib`'s parameter and return are numbers and that its
  name never escapes, so it lowers to `func fib(%0: f64) -> f64` with a
  direct typed call instead of boxing an argument, calling through the
  uniform dynamic convention, and unboxing a result — per call, on a
  function whose body is two additions. LLVM could never see through that;
  now there is nothing to see through.

  property_access barely moves, exactly as predicted: every iteration is
  still two `prop.get` helper calls, and inference's contribution so far is
  only that the loop accumulator is carried honestly. That number is what
  docs/0010 decision 7 attacks — its 1.13x here is the control, not the
  result.
- 2026-08-11 (inline property caches, docs/0010 step 4): property_access
  **85.1/80.6ms — 37% faster** than the entry above; fib 10.7/6.9ms and
  numeric_loop 37.5/33.3ms unchanged (they hold no objects).

  The IC table moved out of a runtime `std::vector` and into a global array
  in the generated object file, so generated code can finally hold a stable
  pointer to a site's entry and do the check itself: tag check, `flags`
  check, shape compare, one indexed load. Instrumented over the benchmark's
  2,000,000 property reads, `bronze_prop_get` is entered **twice** — one
  cold miss per site. The remaining time is no longer dispatch.

  Cumulative over the three benchmarks since phase 3 began: fib **11.1x**,
  numeric_loop **2.4x**, property_access **1.75x**.

- 2026-08-11 (annotations become hints, docs/0010 step 5 — phase 3
  complete): **no benchmark entry, deliberately.** None of `bench/*.js`
  carries a type annotation, so this step cannot move any of them by
  construction — the numbers above stand unchanged. The step's effect is
  the opposite of a speedup where it applies at all: an annotation that no
  proof backs no longer buys a native type, so annotated code that was
  fast *and wrong* becomes dynamic and correct. Code that was fast because
  inference proved it is untouched, which is the entire point of decision
  6.
