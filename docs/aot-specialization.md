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
the invariant later steps lean on. S2 carries a guard's answer across calls
within a function as a layout fact. S4 (built out of order, below S2) made
the IC stubs' hits cheaper. S5's allocator rule is built (under "Measured
against V8"); S3 is not.

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

### S2: layout facts across calls

`src/lower/layout_facts.{h,cpp}` (the design is in the header). A receiver's
static-slot guard, asked once, answers the later static sites on the same
receiver in the same function across calls, getters, stubs and blocks, while
the layout epoch is unchanged.

- **The fact** is an IL value: `layout.fact %r` (`LayoutFact`, f64, impure,
  cannot throw) carries one site's claim (identity cell or family range) and
  evaluates to the layout epoch word when `r` passes that claim's guard, else
  1, which no epoch is (the f64 holds the word's bits, never its conversion).
  A covered site (`PropGet` / `PropSet` with `shapeGuarded` and the fact as
  its last operand) is lowered by il2mir as
  `epoch == fact ? bare slot access : the site's ordinary lowering`
  (`lower_fact_site`). The miss path is the full static site (guard, cache,
  fill), so a failed fact costs one compare more than before and never runs
  anything twice. Covered sites the backend batches into one static run share
  one epoch load.
- **Where facts go.** Per receiver SSA value, an ANCHOR is a static site no
  other site on that receiver dominates. Facts are made in front of an anchor,
  where il2mir's shape memo already has the receiver's keyed shape, so each
  fact is a compare. A member is covered only when it is REMOTE: in a different
  memo stretch from its anchor (a call, a stub or a block edge the memo does not
  cross lies between). Members in the anchor's stretch already get the memo's
  compare and are left to it. A stretch is covered only whole, only when one
  claim answers all of it, and only when that claim is a FAMILY range used in
  two or more remote stretches. Identity claims are left alone: an identity
  guard is one load and one compare against the site's cell, about what the
  fact's check costs (epoch word, fact reloaded across the calls, conversion),
  so a fact for one only adds the cost of making it. Covering identity claims
  cost object_graph +1.65% instructions (`traverseDFS` paid three facts for
  one guard per loop iteration); family-only is neutral there.
- **Handlers.** Dominance is computed over a graph that splits each block into
  an entry and an exit node, handler edges leaving from the entry, so a fact
  made mid-block in a try body is never assumed in its handler.
- **Why the epoch check per site and not a fast/slow copy.** S1's epoch is
  global: any call may bump it, so a copy would need a test after every call
  and a slow copy re-entered mid-function. A per-site `cmp` + branch on a TLS
  word is 3-4 instructions where the guard it replaces is a tag test, a shape
  load and a compare against a cell (identity) or a family range (two loads, two
  compares), and the site's miss path is code that exists anyway.
- **AOT only.** The pass runs in `lowerModule` after the whole-program claims
  are stamped (`optimizeForBuild`), which only `bronze build` does. The JIT
  tiers never see a fact, and their output is unchanged. Coroutine bodies are
  skipped.
- **What it cannot cover.** Sites without a static claim (the material's
  `is*` flags, `materialProperties`: "not-in-prefix") still call the IC stubs.
  Identity claims are one cell per site, so two reads of one receiver at
  different keys never share a fact; only family claims (one range for a
  class's whole layout) do. In three_meshes nearly all shareable claims are
  family claims on `material` and `object`.

Coverage on three_meshes (`--wpo-stats`, `wpo-layout` lines):

| scope | static sites | covered | facts |
|---|---|---|---|
| module | 8845 | 885 | 152 at 150 anchors (224 left local) |
| setMaterial | 29 | 28 | 1 |
| setProgram | 37 | 11 | 3 |
| renderObject (two copies) | 12 / 10 | 7 / 5 | 1 / 2 |
| projectObject (two copies) | 22 / 18 | 8 / 3 | 2 / 1 |
| Object3D.updateMatrixWorld | 30 | 7 | 1 |
| painterSortStable | 14 | 0 | 0 (all reads in one stretch) |
| renderBufferDirect | 0-1 | 0 | 0 |

Measured (with and without the pass, same compiler):

- **Microbench** (8 family-claim reads of one receiver per call, a call
  between each pair): 8.41G to 7.77G instructions, -7.7%, ~6 per covered read.
- **Pure-compute benches** (instructions): mesh_churn_2k 374.53M to 371.46M
  (-0.8%); three_math, object_graph, typed_array_crunch, instanced_mesh_churn,
  fib, numeric_loop, property_access, proto_dispatch(_churn) unchanged (within
  0.01%).
- **three_meshes n=2565**: no change outside noise. cpu ms, order-balanced
  pairs: 7.63 / 7.64 / 7.73 / 7.70 without, 7.87 / 7.74 / 7.67 / 7.65 with
  (the second run of a pair is the slower one either way). Main-thread
  instructions per frame 102.0-105.3M without, 102.7-103.7M with; the IC
  stubs vary by ±1M between runs of one build (`__bronze_prop_get_ic`
  5.4-6.6M, `__bronze_method_call_ic` 4.2-5.2M), more than S2 can move.
- **three_skinned n=410**: cpu 6.80 / 6.60 without, 6.50 / 6.64 with;
  90.1M vs 92.0M instructions in one pair; noise.

Why so little on the scenes: the fact replaces a family guard (~12
instructions: tag, shape, stamp, family base, range) with an epoch compare
(~6: the epoch word, the fact reloaded from its spill slot, the conversion,
compare, branch; il2mir CSEs the conversion within a block). That is ~6 per
covered read, and three_meshes has under ~0.1M covered reads per frame (885
covered sites, most in setMaterial / setProgram / projectObject at about one
activation per object): ~0.5M of 102M at most. The IC stubs (~11M) and the
unclaimed reads behind them are untouched. Annotating setMaterial shows the
facts hold (the miss blocks are cold), so the mechanism works; the scenes'
cost is elsewhere.

What this says about the next steps:

- **S3 (receiver-class function versions).** A version entered through one
  guard needs exactly what S2 now provides: a check that survives calls. But
  the S2 numbers say the guard itself is cheap; a version pays only where it
  turns stub sites (no claim: `materialProperties`, the material's `is*`
  flags, polymorphic geometry reads) into slot reads, ~6-12 instructions
  saved on a stub hit of ~40-60 rather than ~6 on a guard. S3's estimate
  should be counted in stub hits removed, not guards.
- **S4 (non-collecting IC fast paths).** The stubs are the larger and the
  more certain target: ~11M per frame on three_meshes in `__bronze_prop_get_ic`
  and `__bronze_method_call_ic` alone, against ~0.5M S2 could reach. S4
  before S3.

Tests: tests/oracle/cases/layout_facts_mutation.js mutates a covered receiver
between two covered reads in every way S1 lists (delete, accessor redefine,
freeze, seal, preventExtensions, setPrototypeOf, double to string and to
object, overflow reallocation by appends, becoming a prototype, delete and
re-add, read-only redefine), from a call one and two levels deep and from a
getter, across if/else, loops and try (anchor inside and before the try),
with covered stores read back.

### S4: IC stub hits

The read and method stubs were ~11.5M instructions per three_meshes frame
(`__bronze_prop_get_ic` 6.4M, `__bronze_method_call_ic` 5.1M in the
same-session baseline below).

**What the calls are** (a temporary counter patch in the stubs, 300 measured
frames of three_meshes n=2565, per frame):

| stub | calls | how they end |
|---|---|---|
| read | 96.3K | way 0 51.4K, ways 1-3 39.4K, mega table ~4.8K, miss to the helper 0.64K |
| | | own data 31.2K, absent 44.2K, prototype data 19.9K, getter 0.32K, helper 0.64K: 99.3% call nothing |
| method | 79.4K | way 0's shape 72.0K: DIRECT entry 65.7K, SLOT 6.2K; way 1 0.36K; mega, exotic ~0; `bronze_call_method` 7-9K |
| write | 4.3K | own slot 4.3K |

A read hit was ~56 instructions, a method hit ~60 before the call. Callers
(`perf --call-graph=fp`): reads from setMaterial, setProgram's many sites,
setup, `__anon_fn_1418`, refreshUniformsCommon; methods from `__anon_fn_527`,
setProgram, updateMatrixWorld, Frustum.intersectsSphere, Sphere.applyMatrix4,
renderObject, projectObject. Caller side, static (objdump of the image):

| function | instructions | frame-slot operands | calls (to a stub) | `movabs` |
|---|---|---|---|---|
| setProgram | 20296 | 3731 | 433 (161) | 627 |
| updateMatrixWorld | 7391 | 1684 | 211 (98) | 211 |
| projectObject | 7252 | 1247 | 150 (46) | 266 |
| `__anon_fn_1418` | 5718 | 1033 | 155 (65) | 186 |
| setMaterial | 4195 | 672 | 103 (41) | 100 |

About a fifth of the hot functions' instructions touch a frame slot. Two
causes: every call is a GC point, and a tagged value whose interval spans
one lives in its slot for the whole interval (brass's unsplittable-gcref
rule); and brass pools equal 64-bit constants module wide with no
rematerialization, so each `movabs` value holds a register or a slot.

**Built.**

- brass: a call to a function the module declares `nocollect` (or `pure`) is
  no GC point for register allocation either: it does not make the values
  live across it unsplittable (`live_range.cpp`, `call_inst_ids_`) and records
  no stack-map roots (`linear_scan.cpp`, `record_live_gcrefs`). The MIR
  verifier refuses a GC point (a collecting call, an indirect call, a
  safepoint) in a function the module declares nocollect, and the interrupt
  poll pass leaves its loops unpolled. docs/gc_contract.md. Users today:
  `bronze_math_sincos_f64` and `bronze_math_random_f64` (nocollect),
  `bronze_tls_block_addr` and the math kernels (pure); instanced_mesh_churn
  -0.4% instructions from it alone.
- bronze, read: `__bronze_prop_get_ic` is now the stub's hit paths
  (il_prop_get_fast.cpp: ways 0-3, the mega table, own data, absent,
  prototype data 1-4 links up), written for a leaf's registers: tag and
  payload tests are shifts against immediates, so no pooled constant holds a
  register. Anything else calls the old stub, now `__bronze_prop_get_slow`,
  with the same operands. An own way-0 hit is ~40 instructions where it was
  ~56; 8 of them are saves and restores of four callee-saved registers,
  which brass gives any function with a call (no shrink-wrapping), here the
  miss path's.
- bronze, method: the site tests way 0's DIRECT entry inline (plain receiver
  of way 0's shape, the direct bit, the latch's epoch) and enters the latched
  code itself; any other receiver calls the stub from a cold block
  (`lower_method_way0_direct`, il_lowering_direct.cpp). Only in images, at
  sites that are not run-once and have no handler.

**Tried and dropped: the nocollect split at the site.** The plan was a read
site calling a call-free half declared nocollect, then the full stub from a
cold block on its miss, so the caller's references would stay in registers
across the hit. The stubs fell as far (11.5M to ~6M), but callers paid back
half of it: a second call per site, the site's operands kept across the
first, 11% more image code, and frame-slot operands *up* (setProgram 3731 to
4787, setMaterial 672 to 958). The freed registers never showed, because a
value is in its slot for its whole interval if it spans any collecting call,
and in setProgram-sized functions nearly every value does. object_graph
+1.3% instructions, mesh_churn_2k +0.4%. One call to a stub that calls the
full stub itself measured better on every count, so that is what is built.

**Measured**, same session, base = this work's changes taken out (main-thread
instructions per frame, `perf`; cpu ms two runs each):

| | base | nocollect split (dropped) | built |
|---|---|---|---|
| three_meshes instructions | 104.45M | 103.02M, 102.80M | 102.38M, 101.83M |
| three_meshes cpu ms | 7.73, 7.75 | 7.66, 7.70, 7.81, 7.68 | 7.61, 7.56, 7.70, 7.72 |
| read stub(s) | 6.43M | 3.44M + <0.3M | 4.12M, 4.48M + <0.3M |
| `__bronze_method_call_ic` | 5.12M | 2.57M, 2.15M | 1.92M, 2.16M |
| three_skinned instructions | 88.22M | 92.69M, 87.68M, 90.11M | 91.82M, 89.02M |
| three_instanced instructions | 64.08M | 63.53M | (no stub traffic) |

three_skinned moves ±2.5M between runs of one build; its stubs fell 5.8M to
~4.3M, the totals are noise. Pure-compute benches, instructions, base to
built: three_math 109.75M to 109.76M, object_graph 318.45M to 319.15M
(+0.2%), mesh_churn_2k 371.46M to 371.91M (+0.1%), instanced_mesh_churn
548.02M to 546.07M (-0.4%); typed_array_crunch, fib, numeric_loop,
property_access, proto_dispatch(_churn) unchanged.

So: the stubs halved (11.5M to ~6.3M), the frame fell ~2.3M (-2%), and
cpu ms moved less than run-to-run noise (0.05-0.15 ms). The rest of the stub saving went to the inline method arms' code
in their callers and to noise in setProgram (8.3-9.7M between builds).

What this says about the next steps:

- **S5 (tagged values in registers across collecting calls)** is where the
  caller-side cost is: the frame-slot operands in the table above come from
  values spanning collecting calls, which no change to the stubs touches, as
  the dropped split showed. (Since built: the stack scan's pinning makes a
  callee-saved register a sound home for them; see "Measured against V8".)
  Constant rematerialization (the pooled `movabs`) is the
  cheaper half of the same pressure, and shrink-wrapping would take the
  read stub's register saves (8 of its ~40) off every hit.
- **S3 (receiver-class function versions)** removes stub sites. A read hit
  now costs ~40 + the call's setup and a DIRECT method hit is inline, so a
  removed site saves less than S2 assumed; count S3's payoff in the built
  per-hit costs, and expect it to depend on S5 for the callers' spills.
- Other call-free candidates: `__bronze_elem_get_keyed` (0.35M), the write
  stub's own-slot store (its barrier is already a no-GC call), the method
  stub's SLOT arm (6.2K calls a frame).

Tests: tests/oracle/cases/ic_fast_split.js (own data at 1, 4 and 7 shapes;
absent keys appearing on the prototype and the receiver and deleted again;
prototype data 1-4 links up, changed; allocating and throwing getters; own
accessors; method calls mono, mixed, replaced, shadowed, through a slot, on a
non-callable, on undefined and null, in try; allocation between), through
the pin matrix's eight rows. brass tests/unit/test_nocollect_calls.cpp: a
tagged value across a no-GC call keeps its register (x64 and aarch64), one
that also spans a collecting call keeps it too (S5, below), a nocollect callee runs
under the JIT, the verifier's refusals, no poll in a nocollect loop.

## Measured against V8

S1, S2 and S4 each moved three_meshes by ~1-2% where they were meant to
move it by more. So the frame was measured against V8 function by function
before anything else was built.

**How.** Both runs are `perf record -e instructions:u -c 200003
--call-graph=fp --user-callchains` of three_meshes n=2565, on the thread that
runs the scene's JS. bro-aot is restricted to the measured window
(`traceWindow`), as rw.py does. Chromium 156 runs with `--no-sandbox
--no-zygote --js-flags=--perf-basic-prof --interpreted-frames-native-stack`.
Its page has no window marker, so two runs that differ only in measured
frames (300 and 1500) are subtracted and the difference is divided by 1200.
V8's embedded builtins (ICs, the write barrier, sort) are resolved through
the perf map, because they live in the chromium binary's text. Instructions
per V8 instruction come from a `--perf-prof` jitdump run (`perf inject
--jit`). "incl" is callee-inclusive: a sample counts for every function on
its stack, so a callee that V8 inlined and bro did not is compared inside
its caller. Scripts are in the c8 scratch directory: `chrome_perf.py`,
`bro_perf.py`, `fnprof.py`, `chr_diff.py`, `fncmp.py`, `insncat.py`,
`slotkind.py`, `guards.py` and `entry.py`.

**The frame by layer** (M instructions per frame, main or renderer thread):

| | bro-aot | Chromium |
|---|---|---|
| total | 104.9 | 51.0 |
| compiled JS | 76.7 (app.so, of which the stubs are 8.8: prop_get 4.7, method 2.3, sort merge 1.8) | 19.9 |
| engine builtins | runtime entries 1.7, libm 2.2, gc 0.3 | 16.5 (ICs 7.7, write barrier 3.0, sort 2.0, other 3.8) |
| runtime / embedder C++ | bronze runtime 9.4, bro webgl 6.2, libc 3.2 | Blink + GL client + V8 C++ 14.0, libc 0.6 |
| GPU driver | 5.0 (radv, on this thread) | 0 (it runs in the GPU process) |

The gap is in compiled code: 76.7M against V8's 36.4M of JIT code plus
builtins. The native side is ~26M against ~14.6M, and 5M of that difference
is a driver that Chromium runs on another process. Inside the native side:
`glsl::typeInfo` 1.6M, `hostTrampoline` 1.8M, WeakMap lookup 1.6M
(`findFastIdentity` + `weakMapGet`; V8 0.3M).

**Per function** (M per frame; names matched by their last component; bro's
renderBufferDirect is `__anon_fn_1418`):

| function | bro self | V8 self | bro incl | V8 incl |
|---|---|---|---|---|
| renderObjects | 1.36 | 0.78 | 49.6 | 34.1 |
| renderBufferDirect | 3.50 | 1.79 (Maglev) | 42.3 | 30.8 |
| setProgram | 8.91 | 2.48 | 22.9 | 16.1 |
| projectObject | 2.73 | 1.40 | 13.9 | 2.75 |
| ⤷ Frustum.intersectsObject (V8 inlines) | 0.62 | — | 6.3 | — |
| ⤷ intersectsSphere + Vector3.dot (via `__wrapper_`) | 2.15 + 0.90 | — | 3.06 | — |
| uniform upload: setValue, copyArray, arraysEqual, setValueM4/M3, upload | 10.2 | 4.3 (setValueM4/M3/V3f + TypedArray set) | | |
| painterSortStable (+ reverse) | 3.41 + 0.66 | 1.55 + 0.31 | | |
| sort machinery | 1.78 (merge stub) | 2.0 (ArrayPowerSort, MergeAt, SortCompareUserFn) | 5.9 | 4.2 |
| Object3D.updateMatrixWorld | 2.71 | 1.09 | 3.92 | 2.37 |
| Matrix4.multiplyMatrices | 2.41 | 0.85 | | |
| Vector3.applyMatrix4 | 1.93 | 0.59 | | |
| needsUpdate | 2.52 | 0.87 (Sparkplug) | 2.85 | 1.40 |
| setMaterial | 2.02 | 0.86 | 5.96 | 2.08 |
| property / method ICs | 7.0 (two stubs) | 7.7 (IC builtins) | | |
| write barrier | 0.34 | 3.03 | | |

The IC stubs are not the gap: V8 spends as much in its megamorphic and keyed
IC builtins. The gap is the compiled functions themselves, 2-5x V8 each, and
the callees V8 inlines that bro calls: projectObject's subtree alone is
13.9M against 2.75M.

**What bro's compiled instructions are** (sample-weighted, every function
with samples; V8 scaled to its 36.4M):

| category | bro | V8 | gap |
|---|---|---|---|
| frame-slot moves and operands `[rbp-]` | 21.6 | 2.7 | 18.9 |
| NaN-box tag tests / boxing / cmov selects | 8.7 | 0.2 | 8.5 |
| 64-bit constants (`movabs`) | 2.4 | 0.4 | 2.0 |
| bool materialization (setcc, movzx, `and 1`) | 3.3 | 0.1 | 3.2 |
| call setup | 1.4 | 0.3 | 1.1 |
| prologue/epilogue push/pop/ret | 1.1 | 1.2 | 0 |
| branches | 6.4 | 7.6 | -1.2 |
| everything else (loads, stores, ALU, compares) | 31.1 | 23.4 | 7.7 |

The frame-slot traffic, split by what the slot holds (`slotkind.py`):

- tagged and other GPR values: 13.3M (stores 7.3, loads 3.6, folded operands
  2.3);
- callee-saved save and restore in prologues and epilogues: 3.2M;
- pooled constants: 2.4M, stored at entry and read as operands;
- doubles: 2.5M (multiplyMatrices alone 1.0M).

**Mechanisms, in M instructions per frame (they overlap):**

1. **Receiver guards: ~13M.** Every static-slot and IC site guards its
   receiver with the branch-free keyed-shape idiom: untag by `xor`, a range
   compare, `cmovb` to the TLS stand-in, load the header and the shape,
   compare the header, `cmove` to the not-plain shape, then compare a cell
   addressed through the realm's index. That is ~12.7 instructions
   (`guards.py`: 5928 sites, 19.4M per frame, ~1.5M guards). V8's map check
   is `test`/`cmp [obj-1], map` plus two branches, about 4. Most of the
   tag/cmov category and of the bool category is this idiom and the ANDed
   terms that feed its single branch.
2. **Calls V8 inlines: ~10M.** The entry and exit sequences cost 7.0M
   (`entry.py`; the median entry is 26 instructions: frame, four callee-saved
   saves, argument stores, stack check, realm index, pooled constants stored
   to slots). projectObject's subtree (Frustum.intersectsObject →
   intersectsSphere → Plane.distanceToPoint → Vector3.dot through a
   `__wrapper_` indirect call, Sphere.applyMatrix4, Sphere.copy) is 11M over
   V8, which inlines all of it into projectObject.
3. **Tagged values pinned to frame slots across collecting calls (S5): up
   to 13.3M of slot traffic, but only ~2M of it removable by the rule
   alone.** Built (below): a tagged value that spans a GC point may take a
   callee-saved register. There are four allocatable callee-saved GPRs
   (r13 holds the TLS block), and an unsplittable tagged interval gets one
   for its whole life or not at all. The rest needs splittable tagged
   intervals or fewer calls (item 2).
4. **Doubles spilled in numeric code: 2.5M** (multiplyMatrices,
   applyMatrix4, setFromEuler). SysV has no callee-saved XMM registers, and
   these kernels hold 32 doubles.
5. **Pooled 64-bit constants: ~4.8M** (`movabs` 2.4M plus their slots
   2.4M). Most are the guard idiom's tag and limit. Rematerializing them
   saves only the stores at entry, because a folded `xor rax,[rbp-x]` is
   free and a rematerialized constant costs one `movabs` per use. What
   removes them is a guard that compares against immediates (item 1).

Not the gap: the IC stubs (7.0M against V8's 7.7M), the write barrier (V8
pays 2.7M more), and the comparator-based sort machinery (5.9M against
4.2M, all of it comparator bodies).

**Ranked by what one change would remove**, guards come first (item 1,
~13M, plus most of item 5), then inlining and call overhead (item 2), then
S5. S5 was built first anyway, because it was the brief's expected item, is
small and sound, and no guard change measured paid on its own (below). It is
not on main: it moved cpu ms by less than noise and cost the pure-compute
benches 0.3-0.7%, so it is parked on brass branch `s5-tagged-callee-saved`
until tagged intervals are splittable.

### Parked: S5, tagged values in callee-saved registers across GC points

The verifier and the stack walker were not the obstacle. Every collection
that moves or marks first scans the whole thread stack and the captured
callee-saved registers conservatively and pins what they name
(brass `heap_pin.cpp`), and bronze already opts into that retention
(`precise_stack_roots`). So a reference in a callee-saved register across a
collecting call is sound with no stack-map entry and no store or reload at
the call: its object neither moves nor dies while the register (or a callee's
save area, which is a stack word) names it. Only slots are recorded as roots.
The `CalleeSavedReg` root kind (x64 designated slot, spill/reload around
calls) was never produced by the allocator and stays unused. brass changes:

- `linear_scan_pieces.cpp run_scan`: a GC reference live across a call
  competes for a register like any other value (it was forced to its slot).
- `linear_scan.cpp record_live_gcrefs`: a call's roots are the live
  references that are in slots.
- `allocate_blocked`: such a reference, when it holds a register, may be
  evicted to its slot for its whole life by a value that needs the
  register, at the weight of its uses, as one read only by copies is.
  Without this, a receiver taken at entry kept a callee-saved register
  through a hot loop it reads twice per turn, and typed_array_crunch's
  computeForces bounds checks lost theirs (+5.3% instructions).
- `linear_scan_rewrite.cpp`: an x64 compare whose two sides are both in
  slots reloads the left one and reads the right one in place (`mov r10,
  slot; cmp r10, slot`), not two reloads and a register compare.
  Independent of S5, but S5 made such compares hot in computeForces.

Measured, same session, base = these four changes taken out (main-thread
instructions per frame and cpu ms, two runs each):

| | base | S5 rule only | built |
|---|---|---|---|
| three_meshes instructions | 103.48M | 102.31M | 101.49M |
| three_meshes cpu ms | 7.84, 7.58 | 7.58, 7.51 | 7.78, 7.69 |
| setProgram | 9.78M | 9.12M | 8.06M |
| three_skinned instructions | 90.23M | 89.27M | 90.95M |
| three_skinned cpu ms | 6.64, 6.62 | 6.71, 6.51 | 6.61, 6.55 |

three_meshes falls 1.9%, setProgram 18%. cpu ms moves less than run-to-run
noise; three_skinned is noise (±2.5M between runs of one build). Frame-slot
operands, base to built: setProgram 3867 to 3343, copyArray 163 to 154,
setValue 92 to 84, painterSortStable 275 to 268, updateMatrixWorld 1536 to
1517; projectObject 1298 to 1376 and setMaterial 777 to 784 rise (receivers
holding callee-saved registers through straight-line code).

Pure-compute benches, instructions, base to S5 rule only to built: three_math
109.76M to 110.57M to 110.53M (+0.7%), object_graph 319.15M to 320.09M to
320.20M (+0.3%), typed_array_crunch 228.27M to 240.28M to 229.14M (+0.4%),
mesh_churn_2k 371.91M to 372.86M to 372.93M (+0.3%), instanced_mesh_churn
546.06M to 548.05M to 549.42M (+0.6%); fib, numeric_loop, property_access,
proto_dispatch(_churn) unchanged. The residual is spread across numeric
functions (Matrix4.invert, multiplyMatrices: +1.5-3% slot operands, with no
single cause): an unsplittable receiver that takes a callee-saved register at
entry keeps it through straight-line code where splittable temporaries would
use it better. Weighing such a holder by only its register-needing reads
changed nothing and was taken out. Splittable tagged intervals are the fix,
and they need per-piece root liveness in the stack maps: a slot recorded as
a root while its value is in a register piece may still hold a dead object
from an earlier iteration.

**Tried and dropped: a branchy keyed shape in images.** The keyed-shape
idiom as two branches to a cold not-plain join instead of two `cmov`s (three
instructions fewer per guard on the plain path). three_meshes 104.05M with
the S5 rule alone, 104.36M with this: noise. Taken out. (S6 below: one run
of each was inside the scene's run-to-run spread, ±2.5M with the same
`app.so`, so this verdict said nothing either way; the branching form S6
built, with no join, pays.)

Tests: brass `test_gc_tagged_roots.cpp` (twelve tagged references across a
collecting call, some in callee-saved registers and some in slots, under no
stress, minor, alternate and incremental stress), `test_nocollect_calls.cpp`
(a tagged value across a collecting call keeps its register and records no
root), `test_linear_scan_spill_copies.cpp` (six tagged values across a call,
at least one in a slot; a compare of two spilled values reads the right one
in place), all 1738 unit tests with `BRASS_GC_VERIFY=1 BRASS_GC_POISON=1`.
bronze: the oracle's 792 cases infer and no-infer, the pin matrix's eight
rows over fifteen GC-heavy cases, `wpo` and `codegen_brass`.

### S6: receiver guards that branch

**What a guard's instructions were spent on.** `guardparts.py` (c9) splits
every `cmovb`-anchored guard window of the base `app.so` by part, sample
weighted, three_meshes n=2565 (20.1M per frame in those windows, scheduled
neighbours included):

| part | M/frame | what |
|---|---|---|
| header | 4.8 | the 32-bit header load, `cmp e?,0xfff1`, `mov r,2`, `cmove` (mostly the load's cache miss lands here) |
| shape | 3.3 | the shape word's load |
| object test | 2.4 | `cmp` against the pooled limit, `mov r,r13`, `cmovb` |
| reload | 2.2 | the cell table's per-thread base, reloaded from its frame slot |
| bool | 1.1 | `sete`/`movzx`/`and`/`test` where a plain bit is ANDed with a way compare |
| branch, cell, untag, moves | 2.6 | the one `jne`, the cell compare, the `xor` |
| scheduled neighbours | 3.8 | not the guard's |

Two corrections to item 1 above. A quarter of what was counted as receiver
guards (5.2M of 20.1M) is the callee test of closure calls (`entry_test`:
the same select-to-TLS idiom, header `0x2fff1`), not a receiver's shape.
And the scene's instruction count is not repeatable to the 0.3M the earlier
A/B relied on: four perf runs of one and the same `app.so` gave 100.9,
98.4, 96.1 and 98.2M (ip-matched main-thread samples; `copyArray` alone
moves 3.2-5.2M between them). Only deterministic benches, or several runs
a side, can rank a change of a few percent here.

**Design.** Each part asked separately:

- *Not-plain header test.* Needed for an arbitrary receiver: an Array keeps
  `length | capacity << 32` at +8, where a plain object keeps its shape, so a
  shape compare alone can be forged by an Array's counts. It can only go
  where plainness is already known.
- *Object test.* Needed likewise: nothing in the IL types a value as an
  object, and inference is not trusted for soundness.
- *Per-thread cell.* Shapes live in the thread's NonMovingArena and die with
  it, so a process-global cell could compare equal to a new thread's shape
  at a reused address (ABA). A global cell is possible only with the
  runtime clearing a dead thread's publications first; it would save the
  base reload (one instruction) and the delta at entry. Not built.
- *Immediates / patching.* `cmp [ptr+8], imm32` needs shapes in the low 2GB
  and text patched per thread; not worth it against the above.

What the measurement ranked first is the form, not a part: a guard whose
receiver the shape memo does not hold computed the keyed shape with two
selects (to the TLS stand-in, then to 2) and compared once. It now branches
straight to the site's miss: `xor`, `cmp`/`jae` (not an object), `cmp
dword [p],0xfff1`/`jne` (not plain), the shape load, the cell compare and
`jne` — no stand-in, no `2`, and on the hit path the shape word IS the keyed
shape, which the memo keeps (the miss recomputes it, as before). The
difference from the dropped variant is that nothing joins: the refusals go
to the miss block the site already has.

**Built** (bronze il2mir unless named):

- `plain_shape_or_miss` (il_static_slot.cpp): the two tests as branches,
  then the shape word. Used by a lone static read or write
  (`guard_or_miss`, with `StaticMiss::memo_fresh` handing the shape to the
  memo), a static run (il_lowering_static_run.cpp: the slow copy starts from
  the memo as it was before the guard), a direct method call
  (il_lowering_calls.cpp), and the AOT way-0 inline read and write
  (il_ic_lowering.cpp; in-process tiers keep the select form, whose `hit`
  value the speculation sites arm). `static_guard(..., keyed_plain)` drops
  the `keyed != 2` term and reads family stamps and double maps through the
  shape itself.
- `entry_test_br` (il_lowering_direct.cpp): the closure-call callee test
  as branches to the call's slow path, at the closure call, the method
  stub's slot and mega entries, the slot call, the getter/setter stubs and
  inline `new`.
- brass x64 isel: a compare against an immediate whose other side is a
  single-use load in the branch's block reads the load in place (`cmp
  dword [rax],0xfff1`), except an equality with zero, which stays a `test`.
- Unchanged: guards whose answer is an i1 the IL branches on later
  (`is.shape`, `layout.fact`, `method.ic.is`, `accessor.ic.is`) keep the
  keyed-shape selects; they are 0.07M of the remaining executions.

A fresh guard is now about 9 instructions (`xor`, `cmp`, `jae`, `cmp`,
`jne`, load, `lea`/reload, `cmp`, `jne`) against 13.

**Measured** (same session, base = HEAD):

| | base | S6 |
|---|---|---|
| three_math | 109.76M | 107.30M (-2.2%) |
| object_graph | 319.11M | 314.13M (-1.6%) |
| typed_array_crunch | 228.28M | 227.32M (-0.4%) |
| mesh_churn_2k | 371.91M | 363.73M (-2.2%) |
| instanced_mesh_churn | 546.07M | 539.10M (-1.3%) |
| fib, numeric_loop, property_access, proto_dispatch(_churn) | | unchanged |
| three_meshes main thread (ip-matched), M/frame | 100.9, 98.4, 96.1, 98.2 | 93.1, 95.5 |
| three_meshes app.so, M/frame | 80.4, 78.2, 76.3, 77.5 | 73.1, 75.1 |
| three_skinned n=410 app.so, M/frame | 61.0, 62.7 | 61.8, 60.8 |
| three_meshes cpu ms (`rw.py time`, two runs) | 7.56, 7.62 | 7.58, 7.64 |
| three_skinned cpu ms | 6.46, 6.59 | 6.79, 6.57 |

The meshes scene loses ~4M instructions a frame (4-5%) in the main thread
and app.so; skinned, whose hot code is bone math in typed arrays with few
fresh receivers, does not move beyond its spread, and neither scene's cpu
time moves outside noise. (The explanation first given here, that the
removed selects ran in the shadow of the header and shape loads, does not
hold: counted exactly, S6 removes 2.9M instructions and adds 0.32M taken
branches and 10% more backend-stall slots, and its cycles rise 1%. See
"Cycle accounting".)

Categories (`insncat.py`, every hot function, one run a side, M/frame):
tag/box/cmov 9.2 to 6.3, bool 3.9 to 3.0, branches 7.5 to 10.3, `movabs`
2.5 to 2.6. The pooled tag and limit constants stay: the branching form
still reads them as folded frame-slot operands.

Tests: `receiver_guard_branches` (new: every site kind against numbers,
strings, booleans, null/undefined, symbols, BigInts, arrays, typed arrays,
functions, proxies, maps, dates, frozen, reshaped and dictionary objects, a
subclass and a prototype-only object, and shape changes across a call), the
oracle's 807 cases infer and no-infer, the pin matrix's four rows over
fifteen guard cases, bronze `wpo`, `codegen_brass`, `runtime`, and brass's
1736 unit tests with `BRASS_GC_VERIFY=1 BRASS_GC_POISON=1`. No case runs a
second realm or thread against a module's cells; the cells are per thread
and unchanged.

**What remains, ranked.** Guards are now ~9 instructions against V8's 4, and
the rest of the gap is in parts that need a runtime or ABI change: the
not-plain test (an Array header with a shape word at +8 would remove it,
and the object test with it where a type fact holds), the per-thread cell
base (process-global cells cleared at thread exit), and the family stamp's
per-thread base id (a process-wide id range per image would make it a
RIP-relative load). Above guards now: calls V8 inlines (~10M, item 2),
then splittable tagged intervals (S5's 13.3M of slot traffic, ~2M by the
rule alone), then doubles (2.5M), then pooled constants (~2.4M of slot
stores at entry).

## Cycle accounting

S1-S6 each removed instructions and none moved cpu ms, so the frame was
measured in cycles on halo (Zen 5), three_meshes n=2565, base = S6. Totals
are counted (`perf stat -I`) or low-rate samples without call chains, main
thread, bro inside `traceWindow`, Chromium as 1500- minus 300-frame runs
(±10%); per-function shares from call chains, IBS op samples and LBR.

| per frame, main / renderer thread | bro-aot | Chromium |
|---|---|---|
| cpu ms (rw.py, these runs) | 7.7 | 3.3-3.8 |
| cycles; instructions; IPC | 39.6M; 122.2M; 3.08 | 16-18M; 59.0M; ~3.5 |
| on-CPU (task-clock incl. kernel) | 7.88 ms of 7.88 | 3.9 ms |
| ops from the legacy decoder | 65.5M of 121.3M (54%) | 17.1M of 57.4M (30%) |
| op-cache misses / accesses | 11.7M / 21.0M | 3.5M / 10.6M |
| L1i misses; iTLB L1 misses | 12.1M; 0.55M | 4.6M; 0.35M |
| taken branches; all branches | 9.25M; 19.5M | 4.50M; 10.8M |
| calls (= returns); indirect | 1.07M; 0.41M | 0.58M; 0.22M |
| mispredicts | 40K | 37K |
| L1d demand misses (L2 hits); dTLB L1 misses | 1.88M (1.79M); 0.69M | 0.22M (0.15M); 0.41M |
| top-down: retiring / frontend (latency + bandwidth) / backend / bad spec | 36 / 50 (15.5+35) / 11.5 / 2 % | 41 / 38 (17.5+20.5) / 23 / 3 % |

**The frame is the main thread's cycles**: on-CPU 7.883 of 7.884 ms at
5.1 GHz, `js` 7.59 of 7.66 ms, nothing waits. Chromium also spends ~5.6 ms
a frame on other cores (Viz 3.5, thread pool 1.5, GL thread 0.6).

**By layer**: bro app.so 5.3 ms (68%), bronze runtime 1.0 (hostTrampoline,
WeakMap `findFastIdentity`/`weakMapGet`, `taSet`, `rtHandleData`), radv 0.74
(IPC 1.41), libbro 0.44 (`bindVertexInputs`, `setUniformMatrices`,
`prepareDraw`, `PipelineKey==`, `glsl::typeInfo` 0.55M cycles), libc 0.28.
Chromium: JIT code 1.4 ms, the chromium binary 1.9, the driver elsewhere.

**Where bro's 4.4 ms over V8 comes from.** Per function, bro's cycles per
instruction match V8's (setProgram 0.35 vs 0.35, multiplyMatrices 0.50 vs
0.54, projectObject 0.37 vs 0.39); across bro's 48 hottest functions
cycles ≈ 0.36 × instructions (R² 0.905). The frame splits:

- **3.6 ms: 2.07x the instructions** (63M more a frame) at V8's average
  0.29 cycles each.
- **+1.3 ms: frontend bandwidth.** bro's hot code does not fit: the lines
  holding 50/80/90% of its ops are 43/118/157 KB against V8's 10/34/50 KB
  (IBS op samples; app.so 115 KB of the 90% set, runtime 16, radv 13, libbro
  11; setProgram 19.8 KB hot of 102 KB against V8's 4.3 of 17). So 54% of
  ops come through the 4-wide decoder. Hot app.so bytes: frame-slot
  operands 29% (disp32 9.8%), branches 13% (rel32 to out-of-line blocks),
  `movabs` 7%, selects 4.5%; 4.9 bytes an instruction.
- **-0.6 ms: backend.** bro has 8.6x V8's L1d misses (IBS, a frame: JS heap
  and shapes 808K, IC sites in app.so's .data 432K — a 96-byte site per
  property, way 0 first, so each executed site is its own line and
  setProgram's miss every time — malloc'd driver and host objects 323K,
  stack 65K; V8 256K in all), yet fewer backend-stall slots per
  instruction: the frontend starves the backend, which hides them.
  Frontend latency and bad speculation are at parity.

**Why removed instructions did not pay.** S6 again, both app.so builds in one
session, `perf stat -I`: instructions 124.9 → 122.0M, decoded ops 69.8 →
65.5M, but taken branches 8.96 → 9.28M, mispredicts 36K → 40K, backend-stall
slots 34.5 → 37.9M, cycles 39.0/39.1 → 39.3/39.5M. A taken branch costs
about what 5-9 removed instructions save (per-function fit: 0.22 cycles an
instruction, 1.9 a taken branch). Yardsticks, in order: hot code bytes and
taken branches, then instructions; never instruction count alone.

**Taken branches** (LBR, a frame): bro 9.25M — 5.0M inside app.so functions
(setProgram 0.75M against V8's 0.10M), 0.7M inside the runtime; V8 2.2M
inside JIT code and builtins. Of bro's, 0.97M are round trips (a branch to a
block placed after the function's main line and a jump back), 0.59M of them
because the generic helper call (`bronze_unbox_f64`, `bronze_strict_eq`,
`bronze_rel_lt`, ...) fell through; 0.72M are taken skips over a call
(`bronze_stack_overflow` 245K, `bronze_env_get_tdz` 160K). Without a
profile, brass's layout let "the side with work" — the call — fall through.

**Built: the call heuristic in brass's layout** (`codegen/block_layout.cpp`
`shape_score`): of a two-way branch's sides, one that calls and one that
does not, the one that does not falls through. Same session, three runs a
side, `perf stat -I`:

| | base | call heuristic |
|---|---|---|
| three_meshes cycles (M/frame) | 39.05, 38.32, 39.14 | 37.93, 38.09, 37.80 |
| three_meshes cpu ms | 7.56, 7.43, 7.57 | 7.35, 7.39, 7.32 |
| three_meshes taken branches; op-cache accesses | 9.26M; 20.9M | 8.48M; 19.8M |
| three_skinned cycles; cpu ms | 34.1, 34.6; 6.78, 6.67 | 35.2, 34.1, 33.3; 6.78, 6.56, 6.42 |

three_meshes -2.7% cycles, -2.0% cpu ms; three_skinned within its spread;
object_graph -2.6% cycles, fib's taken branches -28% at equal cycles.
Tests: two cases in brass `test_block_layout.cpp`, brass's unit tests under
GC verify/poison, the oracle (infer and no-infer), bronze `wpo`,
`codegen_brass`, `runtime`.

**Ranked from here.**
1. Hot code size (~1.3 ms frontend plus the instruction gap): frame-slot
   traffic (splittable tagged intervals), pooled constants, rel32 tails;
   the 90% hot set from 157 KB toward 60 KB. Continued in [aot-codesize.md](aot-codesize.md).
2. Taken branches left (~1.1M a frame): IC stub way hits out of line,
   the plain-object guard's `je` to an out-of-line shape test.
3. Calls V8 inlines (1.07M calls against 0.58M; projectObject's subtree).
4. bro, outside the compiler: GL on the JS thread, 1.2 ms (radv 0.74,
   libbro WebGL 0.44 in `src/webgl/vulkan/`); Chromium runs it elsewhere.
5. Data layout (IC way 0 packed apart, compact objects): ABI change.

## Measuring

Run from bro:

```
python3 bench/realworld/rw.py time --browser bro-aot --scene three_meshes --n 2565
```

`rw.py` rebuilds `app.so` on every invocation. For per-frame totals, count
(`perf stat -I 250` over the same `bro` command line, steady intervals) or
sample at a low rate without call chains, restricted with `perf script
--time` to the run's `traceWindow`. High-rate `--call-graph=fp` sampling is
throttled by the kernel and undercounts: it put the frame at 96-111M
instructions where counting gives 122M. Compare cycles and taken branches,
not instructions alone.
