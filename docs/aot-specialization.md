# AOT receiver-class specialization

This document covers bronze's AOT-only work to close the gap to V8 on bro's three.js
scenes (`bench/realworld` in bro: `three_meshes`, `three_skinned`, `three_instanced`).
The aim is to specialize hot code on the classes its receivers are predicted to
have. The JIT stays feedback-driven, and nothing here changes it.

The doc records what the profile says the gap is made of, what has been built so
far, what was tried and dropped, and the order in which the rest should be built.

## Where the time goes

The figure of merit is main-thread user instructions per measured frame, recorded
with `perf record -e instructions:u` over a windowed `bro-aot` run, from
`three_meshes` n=2565. The sum below is the baseline before this work (107-108M
instructions per frame). Compiled JS accounts for 78M of it and bronze's runtime
for 15M.

| Cost per frame | Instructions | What it is |
|---|---|---|
| `setProgram` | 10-11M | renderer state checks: ~40 reads of material and object fields, half of them polymorphic (six geometry shapes, four material shapes) |
| `__bronze_method_call_ic` | 5.4M | out-of-line method stub: way misses, mega probe, the call |
| `__bronze_prop_get_ic` | 5.3-5.9M | out-of-line read stub: way-0 misses, prototype members, getters |
| `copyArray` + `arraysEqual` | 4.6M + 1.1M | uniform cache compare and copy over plain Arrays (16 and 9 elements) |
| `Array.prototype.sort` | 3.5M comparator + 3.3M runtime | `painterSortStable` called through `rtArraySortBuiltin` → `rtEnterJs` about 61k times a frame |
| `renderBufferDirect`, `projectObject` | 3.7M, 3.4M | generic traversal and dispatch |

`perf record --call-graph=fp --user-callchains` over the two stubs, grouped by
caller, shows that the stub calls do not come from sites that lack an inline test.
They come from sites whose inline way 0 misses, and from sites whose receivers
rotate through more shapes than the cache has ways. Making every read an inline
way-0 test saved only about 1% (measured, then reverted). The stubs' own costs are
these:

- **Prologue and epilogue.** brass gives every stub a full frame: four callee-saved
  registers are saved and restored on every call, and constants are kept in stack
  slots. That is about 15-20 of the 50-60 instructions on a hit.
- **The mega probe.** For the 4- and 6-shape sites, the stub runs the thread's
  hashed (shape, key) table.

## What is built

### 1. Direct targets for escaped receivers with one known callee

`planDirect` is in `src/opt/wpo/stamp.cpp`. In the predicting solve, a method site
classified `UnknownTarget` with exactly one known target is now planned like a
`Single` site. An example is `planes[i].distanceToPoint(p)` in
`Frustum.intersectsSphere`: the planes escape into an array the analysis cannot
see through, but their prototype is still predicted.

This is sound because of how a planned target is entered:

- The site's guard is its method cache (`direct_method_ic_hit`: way 0 or 1 plus
  the prototype epoch).
- The cache names the callee only for a receiver whose real lookup found it.
- Any other receiver takes `bronze_call_method`.

Some receivers defeat the prediction: one carrying its own method, one whose
prototype method was replaced or deleted, one of another class, or a non-object.
Each of these misses the guard and gets the language's call.
`tests/oracle/cases/direct_unknown_target_escape.js` exercises all of them.

Once the site has a target, guarded inlining (`inline_guarded.cpp`) takes the
callee into the loop. On `three_meshes` the method stub fell from 5.4M to 4.0M
instructions per frame, and compiled JS from 79.5M to 77.2M.

### 2. Truthiness: object and nullish branches before the classifier

`lower_dynamic_truthy` is in `il2mir/il_lowering_ops.cpp`. It already took Booleans
on a branch of their own. Two more branches now come before the branch-free
~35-instruction classifier:

- an Object tag answers true;
- `null` or `undefined` answers false.

Both are presence tests (`if (material.map)`, `if (object.parent)`), the next most
common dynamic conditions in three.js. The answer for every value is unchanged:
bronze has no `[[IsHTMLDDA]]` object.

### Measured

These are windowed `bro-aot` runs. Each "before" figure is the same tree with both
changes reverted by a temporary patch. Instructions are main-thread user
instructions per frame.

| Scene | cpu ms before (2 runs) | cpu ms after (2 runs) | ins/frame before | ins/frame after (2 runs) | Chromium |
|---|---|---|---|---|---|
| three_meshes n=2565 | 8.07, 8.10 | 7.64, 7.56 | 107.4M (108.2M earlier) | 102.9M, 102.2M | 3.2-3.7 |
| three_skinned n=410 | 6.63, 6.39 | 6.30, 6.30 | 89.5M | 88.5M | 2.2 |
| three_instanced n=42992 | 4.81, 4.83 | 4.82, 4.83 | 64.3M | 64.2M | 4.4 |

`three_instanced` is bound by `sin`/`sincos` in the runtime's libm (31M of 64M) and
by a single typed loop. Neither change touches it.

## Tried and dropped

- **Key-owner family guesses at every receiver** (`lower_key_owner_guess.cpp`
  limited to reads of a proven class field). Guessing for any receiver was wrong
  often enough that instructions rose from 108.2M to 113.0M and the read stub
  doubled.
- **An inline way-0 read at every property site.** It saved about 1%, for the
  reason given above. Not kept.
- **Inlining own-slot callees into `setProgram`-sized callers.** This was already
  measured +2.2% (worse) before this work, because brass spills across the
  enlarged body. Guarded inlining's 800-IL caller cap stays.

## What remains, in payoff order

Estimates are instructions per frame on `three_meshes`. They are not additive with
certainty.

1. **`Array.prototype.sort` with a known comparator, specialized in AOT: ~4-5M.**
   - The site `list.sort(painterSortStable)` compiles to a version guarded by
     "the receiver is a plain Array, `sort` is the intrinsic, and the comparator
     is that function".
   - The version runs a bottom-up merge sort written in IL. It snapshots into a
     rooted block as `rtArraySortBuiltin` does, then writes back.
   - The comparator is called directly and can then be guarded-inlined.
   - The 23.1.3.30 rules (undefined last, ToNumber of the answer with NaN as 0,
     a hole-skipping snapshot, frozen and sealed refusals) are taken from the
     runtime path. Any receiver that is not a plain dense Array takes the
     builtin.
   - This removes `hostTrampoline`/`rtEnterJs` per comparison (~3.3M) and lets
     the comparator's field reads stamp. About 400-600 lines.
2. **A fast/slow split for the IC stubs: ~2-3M.**
   - brass has no shrink-wrapping and no tail calls, so every stub hit pays the
     full prologue.
   - Option one: give brass shrink-wrapping for leaf-like fast paths. This is
     generic and also helps every small accessor.
   - Option two: split each stub into a frame-less fast function (ways 0-3 and
     own or prototype data, nothing live across a call) that calls a slow
     function holding the mega probe, the getter entry and the helper.
   - About 300 lines in il_lowering_direct.cpp for option two. Shrink-wrapping
     in brass is larger, but it pays everywhere.
3. **Plain-Array loop versions: ~2.5-3M (`copyArray`, `arraysEqual`, the
   uniform caches).**
   - This is the `typed_loop_version.cpp` model applied to dense Arrays: one
     entry test per receiver (plain Array, no indexed chain elements), then
     element ops that skip the tag, kind and integer-index ladder.
   - The length and element base are re-read at every access unless the loop
     cannot collect.
   - It needs IL ops for dense-array element get and set (hole check kept; a
     store into an existing element needs only the barrier), plus the version
     pass. About 600-800 lines.
4. **Function versions on receiver class (the core of the original plan):
   uncertain, possibly 5-10M across `setProgram`, `renderObject` and
   `renderBufferDirect`.**
   - Clone a hot function per predicted (receiver class, material class) pair
     and enter it through one guard at entry.
   - Inside a version, every read of the guarded objects is a static slot.
   - What blocks it today: a call ends a guard region's cover, because any call
     may reshape the receiver. `setProgram` makes a call every few reads.
   - A version needs a **layout epoch**: a per-shape "has transitioned since"
     counter, or a per-class "no instance added a field after construction"
     fact from the predicting solve's not-in-prefix census. With either, a cover
     survives calls that cannot reshape. Without it, versions fall back to
     re-guarding after each call, which is roughly what the inline caches already
     do.
   - About 1.5-2.5k lines. The epoch's invalidation must be proven under the
     gc-stress and `BRASS_DEOPT_STRESS` oracle runs.
5. **Polymorphic sites as inline 2-4 way tests instead of a stub call.** This
   only pays once item 2 has made a stub hit cheap enough that the inline code's
   size, and the register pressure it adds in large callers, is the larger cost.
   Measure it after item 2.

## Measuring

Run from bro:

```
python3 bench/realworld/rw.py time --browser bro-aot --scene three_meshes --n 2565
```

`rw.py` rebuilds `app.so` on every invocation. For instruction counts, run
`perf record -e instructions:u -c 200003` over the same `bro` command line and
restrict `perf script --time` to the run's `traceWindow`.
