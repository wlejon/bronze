# 0033 — Where the compile time goes, and the frame that was 300x too big

Status: implemented.

Compiling the 28-file three.js graph took ~80 s and nothing measured it. It
had shaped the test suite already — docs/0031 decision 3 puts the milestone
outside `cases/` and behind its own label because four compiles of it would
double the suite — so the number was steering decisions while being nobody's
number. This is the instrument, what it found, and the one change that came
out of it.

## 1. `--timings`, and why a duration is allowed to be nondeterministic

`bronze build --timings` prints wall time per phase to stderr:

```
  load               86.4 ms
  infer            2065.7 ms
  lower            1652.4 ms
    il-verify          24.4 ms
    ir-build          441.3 ms
    llvm-verify       573.0 ms
    obj-emit        74514.6 ms
  codegen         76692.4 ms
  link               91.6 ms
  total           80588.7 ms
```

docs/0001 decision 10 says deterministic output only, and a duration cannot
be. The rule survives intact because of what it is *for*: it constrains what
bronze emits as an artefact — IL dumps, diagnostics, float formatting,
enumeration order — so that a test can compare bytes. A timing is opt-in,
goes to stderr, and is compared by nothing. It is the same line docs/0030
decision 1 drew for `Math.random`, one level up.

The four indented lines are inside the backend, which is reached through
`codegen::Backend`. Rather than put a debugging parameter into the interface
every future backend would have to implement, the flag is a process-global in
`support` that the CLI sets — a narrow exception, and named as one.

## 2. The answer, immediately: it is not bronze

| phase | ms | share |
| --- | --- | --- |
| load (lex + parse + module graph) | 86 | 0.1% |
| infer | 2066 | 2.6% |
| lower | 1652 | 2.0% |
| **codegen** | **76692** | **95.2%** |
| link | 92 | 0.1% |

and inside codegen, `obj-emit` — `addPassesToEmitFile` plus `pass.run` — is
74.5 s of the 76.7. Everything bronze wrote itself, including building the
LLVM IR, is 4.2 s.

Worth stating plainly because it retires a plausible plan: there is nothing
to gain in the lexer, the parser, inference or lowering. A 2x on all four
would save 1.9 s of 80.

## 3. Two levers measured before either was believed

**The codegen optimisation level.** `createTargetMachine` was taking the
default `CodeGenOptLevel::Default`. Dropping it is dramatic and asymmetric:

| level | obj-emit | total |
| --- | --- | --- |
| None | 10.1 s | 15.2 s |
| Less | 75.3 s | 80.4 s |
| Default | 75.3 s | 80.4 s |

`Less` buys nothing at all — the whole cliff is between None and everything
above it. And the runtime cost of None is concentrated in exactly one place:

| benchmark | Default | None |
| --- | --- | --- |
| fib | 7.43 ms | 7.78 ms |
| property_access | 52.00 ms | 53.11 ms |
| proto_dispatch | 204 ms | 204 ms |
| typed_array_loop | 995 ms | 1016 ms |
| **numeric_loop** | **33.65 ms** | **82.45 ms** |

Everything dynamic is a call into the runtime, which is compiled separately
at full optimisation, so LLVM's machine passes have almost nothing to chew
on. A tight proven-f64 loop is the opposite, and 2.45x on it is 2.45x off the
thing docs/0001 decision 4 exists to deliver. **So the lever is recorded and
not pulled.** A `--fast-compile` flag would be a 5.3x iteration win for
anyone who accepts that trade; it is a decision for whoever needs it, not a
default.

**Per-function `optnone`** does work — marking every function cut obj-emit
from 74.1 s to 27.4 s, so LLVM does honour it through the codegen pipeline —
but the obvious predicate does not pay. 52% of three.js's 828 functions
contain no native-typed value at all and would be safe to mark, and those
functions hold **10% of the IL**. The cost is in the big functions, and every
big function mixes dynamic and native values.

## 4. The real finding: the root frame had a slot per value ever computed

A synthetic pinned it. The same 4000 property reads:

- in ONE function: obj-emit **156.5 s**
- split across 40 functions: obj-emit **64.4 s**

Identical work, 2.4x apart, so something is superlinear in function size. And
both are absurd — 4000 property reads costing as much as all of three.js. Per
statement, with everything else held constant:

| 2000 statements of | obj-emit |
| --- | --- |
| proven-f64 arithmetic | 2.0 s |
| a call to a JS function | 23.6 s |
| a property read | 50.4 s |

It is not the inline property cache: `--no-infer` turns every site into a
plain call and the number does not move (159 s against 156 s). What
distinguishes dynamic statements from numeric ones is the GC root frame, and
`planRootFrame` gave **every Dynamic-typed value in the function its own
permanent slot**, monotonically, with no reuse:

- the 2000-statement function got a **6002-slot** frame — a 48 KB alloca,
  6002 unrolled initialising stores in the entry block, and 6002 stack
  locations for the register allocator to colour;
- three.js's `main` got 2320.

That is a frame proportional to how many values the function ever computes,
where the collector only ever needs the ones live at once.

## 5. Slot reuse, and the narrow rule that makes it safe

Slots are now released when the value in them dies. The eligibility rule is
deliberately narrower than real liveness:

- A value used **outside its defining block** keeps a slot to itself, as does
  every block parameter and function parameter. Deciding those properly needs
  liveness over the CFG — a loop header's parameter is live across the back
  edge — and a wrong answer is a use-after-move that surfaces only under GC
  stress, which is this project's most expensive bug class (five instances,
  docs/0031 decision 7 and docs/0032 decision 6).
- Everything else is a temporary whose whole life is one block, and a linear
  scan over that block is exact: within a block a def precedes every use, so
  the range is `[def, last use]` in textual order.

Three details are load-bearing:

- **A slot is released one instruction late.** The operands of instruction
  `k` are loaded out of their slots before its result is stored, so a slot
  freed *at* `k` and handed to `k`'s result would be read after it had been
  overwritten. Slots freed at `k` become available at `k+1`.
- **The pool is shared across blocks.** Two block-local values in different
  blocks can never both be live, so the pool only has to be as deep as the
  worst single block.
- **Block-argument lists are uses.** They live in `inst.target.args` and
  `inst.elseTarget.args`, not in `inst.operands`, and the first version of
  the scan read only `operands` — which would have pooled a value the branch
  still needed and handed its slot to the next def. Every field on an
  `Instruction` that names a value has to be in the scan, and there are three.

A freed slot is not cleared. It holds a dead-but-valid Value until the next
def overwrites it, so a collection in between forwards one object the program
can no longer reach — the same one cycle of float the frame already had, not
a new hazard.

## 6. What it bought

| | before | after |
| --- | --- | --- |
| three.js, total | 80.6 s | **64.6 s** |
| three.js, obj-emit | 74.5 s | 59.4 s |
| 2000 property reads in one function, obj-emit | 50.4 s | **16.7 s** |

19% on the real target and 3.0x on the pathological shape, with the pinned
three.js expectation unchanged and the full suite green under
`BRONZE_GC_STRESS=1`.

The gap between those two numbers is the remaining work, and it is named
rather than guessed at: three.js's functions are smaller and a much larger
share of their values cross a block boundary, so the conservative rule in
decision 5 pins them. Closing that means real liveness over the CFG, which is
its own chunk — and it now has a benchmark, a profile, and a number to beat
rather than a paragraph.

## 7. What was measured and deliberately not done

- **`--fast-compile`** (decision 3): 5.3x, at 2.45x on proven-f64 loops.
  Available, costed, not taken by default.
- **Splitting large functions in lowering.** The superlinearity in decision 4
  is real, but the fix is to make frames small rather than to make functions
  small — and slot reuse addresses the cause where splitting would only move
  it.
- **An IR optimisation pipeline.** There is none: `addPassesToEmitFile` runs
  the codegen passes only, and docs/0002 step 2's "LLVM's default -O2
  pipeline does the work" was never actually wired. Adding one would cost
  compile time to buy runtime, which is the opposite of this doc's question,
  and it deserves its own measurement rather than being smuggled in here.
