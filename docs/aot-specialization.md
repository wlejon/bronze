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

### 3. `Array.prototype.sort` merged in compiled code

A site `arr.sort(f)` in an image build tests that the receiver is an Array
with no properties object, which is what makes `sort` the intrinsic (the
`push`/`shift` arm's test, `il_array_method.cpp`), and calls
`bronze_array_sort_merged` with the address of the module's merge stub
`__bronze_sort_merge` (`il2mir/il_array_sort.cpp`). Any other receiver takes
the site's ordinary method call.

- The runtime (`runtime/builtin_array_sort.cpp`) is the builtin unchanged
  around the merge: comparator validation, the hole-skipping snapshot into a
  rooted block, and the write-back with its frozen and sealed refusals. Only
  the merge is handed over. The stub is entered once per sort, through
  `rtEnterJsIn`, with a frame (`BRONZE_ABI_SORT_FRAME_*`) of root addresses.
- The stub is the C++ merge's loop, run by run: the same comparisons in the
  same order (a comparator that logs sees the same log), undefined last
  without a call, ToNumber of the answer with NaN as equal, and ties to the
  left. It calls the comparator as compiled code calls a closure, through the
  direct entry when the function allows it, else `bronze_dynamic_call`.
  Whether it may enter directly is asked once per sort.
- What this removes per comparison is `rtEnterJsIn`, the C++ entry scope, the
  TLS trampoline and, in bro, the tiered engine's enter hook. The comparator
  still runs through its wrapper. It is not inlined: the stub is shared by
  every comparator in the module, and three.js's comparators are too large
  for inlining to pay anyway (~200 instructions a call on `three_meshes`,
  mostly polymorphic field reads).
- On a standalone build of a 1924-item painter sort, cycles fell 11-16% on
  shuffled lists and 20% on an already-ordered one. The stub's pick is a branch
  and not a select: the select form was 6% faster on shuffled painter lists
  and 19% slower than the C++ merge on ordered ones.
- `tests/oracle/cases/array_sort_compiled_merge.js` covers stability,
  undefined and holes, throwing and mutating comparators, comparators that are
  not functions, receivers that are not plain Arrays, the three.js comparators,
  and the stub's comparison order against the builtin's.

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
   Built as section 3 above, without the comparator specialization: the merge
   runs in compiled code and the per-comparison runtime entry is gone, but the
   comparator is not inlined. What follows is the original plan.
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

## Structural plan

Six steps toward Chromium's ~58M instructions per three_meshes n=2565 frame
(bro AOT: ~102M, 7.85 cpu ms). S0 measured where receivers are read; S1 built
the invariant later steps lean on. S2 onward (dataflow over "layout still
valid", fewer guards and stubs per receiver) is not built.

### S0: what the hot receivers look like

A shape census over 20 warm and 30/60 measured frames (docs/shape-census.md),
joined with `--wpo-stats` by `bench/tools/census_receivers.py`. Accesses per
activation; shapes seen at the receiver's sites (top shape's share); calls and
slotless IC stubs between consecutive reads of the same receiver; the
predicting solve's class.

| function (acc/act) | receiver | acc/act | shapes (top) | calls between | stubs between | prediction |
|---|---|---|---|---|---|---|
| setProgram (81.4) | materialProperties | 21.0 | 4 (1.00) | 1.0 | 2.6 | not-in-prefix |
| | material | 13.4 | 8 (0.38) | 2.4 | 6.4 | not-in-prefix, some same-slot-escaped |
| | object | 9.1 | 1 | 5.8 | 5.1 | absent / same-slot-escaped |
| | geometry | 6.0 | 6 (0.17) | 1.5 | 11.7 | same-slot-escaped |
| renderObject (12.2) | object | 6.0 | 1 | 0.6 | 1.0 | family |
| | material | 2.25 | 8 (0.39) | 1.5 | | |
| renderBufferDirect (25.7) | object | 5.0 | 1 | 2.25 | 2.75 | absent |
| | geometry | 4.0 | 6 | 6.7 | 8.3 | |
| | material | 2.0 | 4 (0.37) | 13 | 12 | |
| projectObject (24.2) | object | 11.35 | 5 (0.96) | 0.7 | 0.3 | family |
| setMaterial (22.2) | material | 15.2 | 4 (0.39) | 1.2 | 0.3 | same-slot-escaped |
| Object3D.updateMatrixWorld (52) | this | 13 | 7 (0.91) | 0.3 | 2.8 | 14/15 sites lowering-stamped |
| painterSortStable (10.5) | a, b | 4.3 each | 1 | 0 | ~1.4 | identity |

- **Leaves in steady state: zero that break a layout.** Every non-append leave
  and every proto-epoch bump happened in the first second (load). In measured
  frames, ~1 leave per frame, all appends on the bench harness's own fresh
  `{q, i}` literals. A layout check that only appends never fail is therefore
  free of churn on this scene.
- **Receivers are read many times with calls between.** A material or geometry
  is re-guarded after every call and every slotless stub (up to ~12 between
  reads), although the object never leaves its shape: the re-guards are what
  S2 removes.
- **Predictions exist** for most sites: the dominant class is
  "same-slot-escaped" (a slot claim exists, but the receiver escaped and "its
  shape may grow"), which is exactly what append-safe layout facts answer.
  "not-in-prefix" (materialProperties, the material's `is*` flags) has no claim;
  runtime-observed shapes (the census, `--spec-profile`) would give one.
- Materials and geometries are polymorphic (4-8 shapes); the fact S2 can carry
  is "the shape checked at the first read still holds", not one global class.

### S1: stable layouts

A shape is WATCHED once compiled code may rely on its layout (shape.h
`watched`): published static-shape cells (static_shape.cpp) and class-family
claims (class_family.cpp) watch the shape and its whole transition subtree, and
a child created under a watched node is born watched. Every runtime path that
moves an object off a shape calls `noteShapeLeave` (runtime/shape_leave.h);
when the shape left is watched and the move breaks its layout (generalize,
dictionary conversion by delete / redefine / integrity / setPrototype /
threshold, become-prototype), `layoutBroken` bumps the layout epoch.

**The check is a global epoch, and it is the prototype-mutation epoch.**
`proto_epoch` is one TLS word compiled code already reads; no ABI change.
IL op `layout.epoch` (`LayoutEpoch`, f64, impure, cannot throw) lowers to a
pinned TLS load; "still valid?" is `cmp.eq` against the value read at the
guard. A call between two reads does not let them merge (codegen_brass test).

Why not the alternatives:

- **Per-shape "left" bits.** Testing a bit on the receiver's shape costs a
  shape load and a test, the same as re-guarding the receiver.
- **Validity cells** (one cell per shape, dependents lists). One load cheaper
  than the epoch where several shapes are relied on, but invalidation is
  permanent and re-arming without ABA needs fresh cells. S0 shows no churn to
  justify it; revisit if S2 measures epoch churn.

Appends never bump. A child keeps every slot of its parent at the same index,
attributes and representation, so compiled transitions (il_ic_lowering's
transition store, il_transition_run, il_define_add) need no hook and
constructors pay nothing. Measured over object_graph, mesh_churn_2k,
instanced_mesh_churn, three_math, property_access, proto_dispatch(_churn) and a
constructor-churn microbench: within 0.01% of instructions of a build without
the hooks.

**What S2 may assume** after a guard of receiver `o` at shape `S` and an equal
epoch: `o` still has every slot `S` described, at the same offset, attributes
and representation. **Not**: that `o` is still exactly at `S` (it may have
appended), that a key absent from `S` is still absent, that a transition store
from `S` is still valid, or anything about objects not guarded.

**Hazard.** A program that breaks watched layouts in a loop (deletes on hot
objects, repeated freezes) bumps the epoch every iteration, which also thrashes
the proto caches sharing it. The census's `epoch_bumps` names the callers.

Tests: tests/runtime/stable_layout_test.cpp (watch propagation, which moves
bump and which do not); tests/oracle/cases/stable_layout_leaves.js (every leave
kind inside a call between two reads, including two calls deep) and
stable_layout_constructors.js (appending constructors, a prototype setter
appearing mid-run, deletes and repr changes inside constructors and callees).

## Measuring

Run from bro:

```
python3 bench/realworld/rw.py time --browser bro-aot --scene three_meshes --n 2565
```

`rw.py` rebuilds `app.so` on every invocation. For instruction counts, run
`perf record -e instructions:u -c 200003` over the same `bro` command line and
restrict `perf script --time` to the run's `traceWindow`.
