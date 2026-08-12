# Benchmarks

Six programs, run by `run_benchmarks.py`, timing a bronze-built executable.
`node` is not invoked — these measure bronze against its own history, and the
trend line is the point.

```
python bench/run_benchmarks.py
```

| Program | What it measures |
|---|---|
| `fib.js` | recursive `fib(30)` — call overhead on a tiny all-`dynamic` function |
| `numeric_loop.js` | 10M-iteration float loop — proven-f64 arithmetic |
| `property_access.js` | 1M iterations of `o.a + o.b` — own-property dispatch |
| `proto_dispatch.js` | depth-3 inherited read, no adds in the loop |
| `proto_dispatch_churn.js` | the same read with `new Pt(i)` per iteration |
| `typed_array_loop.js` | element access on a typed array view |

The last two are a **pair**, and neither number means anything alone. The gap
between them is what a cached depth-3 hit costs over a depth-0 one: ~80ms when
the cache invalidation rule is right, and 857ms when it is too coarse. A change
that regresses proto caching shows up as that gap widening while
`proto_dispatch.js` stands still.

## The log

One entry per change that can move a number, each measured against the one
above it, so a regression cannot hide in an average.

This is a record of measurements, not of work — a number bronze produced on
this machine at a point in time. It is the one place in the repo where "what it
used to be" is the content rather than a smell, because a trend line is the
only way to notice a slow regression that no single run would fail.

- **honest baseline** (control flow landed, benchmarks rewritten as real loops;
  the earlier entries were dropped because the programs were straight-line code
  LLVM constant-folded, so they mostly measured process startup):
  - fib — 112.0ms avg / 105.7ms min
  - numeric_loop — 86.9 / 81.8ms
  - property_access — 890.1 / 876.0ms. ~0.9µs per iteration of two `prop.get`
    helper calls plus boxing — the number inference and the IC fast paths exist
    to attack.
- **out-of-line slots, interned property keys**: fib 118.4/113.4, numeric_loop
  88.1/82.8 (both noise), property_access **144.5/138.1 — 6.2x faster**. The
  890ms was mostly self-inflicted allocation: every property access built a
  fresh key string (~48MB of garbage over the run). Keys are interned once at
  registration, and the IC-hit path now touches no key, no `std::string` and no
  root registration.
- **generated code rooted**: fib 118.3/113.0, numeric_loop 87.4/81.9,
  property_access 148.4/143.1. Cost of surviving a moving collection:
  **nothing** on the two f64 benchmarks — they hold no `dynamic` values, so they
  get no root frame at all — and **~3%** on property_access. The first
  implementation registered each frame with a pair of helper calls and cost
  **2.1x on fib** (248ms), because a tiny hot all-`dynamic` function pays a
  fixed per-call cost twice; linking the frame inline put it back exactly.
- **inference landed**: fib **10.7/6.6ms (11.3x)**, numeric_loop **37.7/33.4ms
  (2.4x)**, property_access 134.0/126.9 (1.13x). Byte-identical output.

  The two numeric benchmarks are the whole argument for inference in one line.
  Codegen did not change: inference proves `fib`'s parameter and return are
  numbers and that its name never escapes, so it lowers to
  `func fib(%0: f64) -> f64` with a direct typed call instead of boxing an
  argument, calling through the uniform dynamic convention, and unboxing a
  result — per call, on a function whose body is two additions.

  property_access barely moves, as predicted: every iteration is still two
  `prop.get` helper calls. Its 1.13x is the control, not the result.
- **inline property caches**: property_access **85.1/80.6ms — 37% faster**; fib
  10.7/6.9 and numeric_loop 37.5/33.3 unchanged (they hold no objects).

  The IC table moved out of a runtime `std::vector` and into a global array in
  the generated object file, so generated code can hold a stable pointer to a
  site's entry and do the check itself. Instrumented over the benchmark's
  2,000,000 property reads, `bronze_prop_get` is entered **twice** — one cold
  miss per site. The remaining time is no longer dispatch.

  Cumulative since inference began: fib **11.1x**, numeric_loop **2.4x**,
  property_access **1.75x**.
- **annotations became untrusted hints**: **no entry, deliberately.** No
  `bench/*.js` carries an annotation, so this cannot move a number by
  construction. Where it applies at all its effect is the opposite of a speedup:
  an annotation no proof backs no longer buys a native type, so code that was
  fast *and wrong* becomes dynamic and correct.
- **the prototype-mutation epoch**: fib 13.5/7.4, numeric_loop 41.2/33.7,
  property_access 60.8/52.0 — all at or inside their previous minima, which is
  the expected answer: the fourth cache word is read only at depth > 0, and
  these three are either pure f64 or an own property generated code inlines.

  **`proto_dispatch.js` and `proto_dispatch_churn.js` join here, and they are
  the entry.** Nothing in `bench/` measured an inherited read before, so a
  change that switched proto caching off entirely could not have moved a number
  — and the first version of this work did exactly that. Baselines, 3M
  iterations, best of five: proto_dispatch **233ms**; proto_dispatch_churn
  **1960ms**, against 1880ms for the identical loop reading an own property.
- **GC root slots reused** — a **compile-time** entry, the first in this log.
  Every benchmark's runtime is unchanged; what moved is how long bronze takes to
  produce them.

  three.js, 28 files: **80.6s → 64.6s**, of which object emission is 74.5 →
  59.4s. A synthetic 2000 property reads in one function: **50.4s → 16.7s**.

  `bronze build --timings` found that 95% of a compile is LLVM's object emission
  and 5% is everything bronze wrote — so the lexer, parser, inference and
  lowering are together not worth optimising (a 2x on all four saves 1.9s of
  80). What was worth it was the root frame: it held a slot per `dynamic` value
  the function ever computed rather than per value live at once, so a
  2000-statement function allocated 6002 of them and handed the register
  allocator 6002 stack locations.

  Measured and **deliberately not taken**: `CodeGenOptLevel::None` is a further
  5.3x on compile time and costs **2.45x on numeric_loop** while leaving the
  others at noise. Trading the one benchmark that is the point of the project
  for compile speed is not a default.
