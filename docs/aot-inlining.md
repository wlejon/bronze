# AOT inlining and call overhead (chunk 13)

The follow-on to [aot-codesize.md](aot-codesize.md). bro makes 1.07M calls a
frame on three_meshes n=2565 against V8's 0.58M, and V8 inlines the frustum
test (`Frustum.intersectsObject` → `intersectsSphere` → `Plane.distanceToPoint`
→ `Vector3.dot`, `Sphere.copy`, `Sphere.applyMatrix4`) into `projectObject`,
where bro's subtree is 13.9M instructions a frame against V8's 2.75M
([aot-specialization.md](aot-specialization.md), "Measured against V8"). This
chunk measured the call edges and what a call costs, built the inlining that
reaches those callees, and measured it in cycles. It did not pay; what was
learnt, the one bug it turned up, and the ranking that follows are below.

Machine and method as in the files before: halo (Zen 5), `perf stat -I`
counting runs over the steady window, variants round-robin with a rest
between runs, three rounds a side.

## The call edges

LBR filtered to calls (`perf record -j any_call,ind_jmp,u`), main thread,
inside `traceWindow`, each entry named by the symbols at its two ends and the
encoding of its call instruction; scaled to the counted near returns
(`ex_ret_near_ret`, 1.078M a frame). A stub or wrapper hop is folded into the
edge it continues. App code to app code is 0.43M of the 1.07M; the rest is
the runtime (0.15M), libbro (0.13M), radv (0.08M) and the crossings between
them.

| how app code calls (M edges/frame) | |
|---|---|
| direct call of a body | 0.163 |
| the property-read stub `__bronze_prop_get_ic` | 0.097 (setProgram alone 0.045) |
| indirect call of `__wrapper_X`, which calls X (method IC DIRECT hit inline, closure call) | 0.076 (each 2 calls) |
| runtime and native entries | 0.104 |
| the comparator from `__bronze_sort_merge`, through its wrapper | 0.026 (each 2 calls) |
| `__bronze_method_call_ic` → wrapper → body | 0.017 (each 3 calls) |

The per-object edges are ~2.6K a frame each (one per mesh). The largest:
`intersectsSphere` → `Vector3.dot` through the wrapper 14.6K (six planes),
`updateMatrixWorld` → `multiplyMatrices` 9.7K and → itself through the wrapper
8.2K, `setProgram` → `WebGLUniforms.setValue` through the wrapper 7.3K, the
sort comparators 25K, then `projectObject` → `Layers.test`, → itself (a
closure call through the wrapper), → `intersectsObject` → `Sphere.copy`,
`Sphere.applyMatrix4`, `intersectsSphere`, and `renderObject` →
`onBeforeRender`/`onAfterRender` (empty bodies, through the wrapper),
`getNormalMatrix` → `transpose`/`invert`/`setFromMatrix4` → `set`.

Why each was a call in the base:

- `projectObject` is a closure that recurses through its own binding
  (`call.dynamic` of an env slot: a self-call is never a stable-slot edge,
  `lower_static_call_plan.cpp`), so nothing marked it hot, and outside its
  children loop guarded inlining copies nothing. `intersectsObject`,
  `Layers.test`, `Sphere.copy`/`applyMatrix4` are resolved sites
  (`directTarget`, WPO's prediction for `this.intersectsSphere`).
- `this.normal.dot(point)` (inside `distanceToPoint`, itself inlined into
  `intersectsSphere`) has no target: four classes declare `dot`, the WPO
  solver sees `normal` as a field of an escaped `Plane`, and the field-class
  guess is refused (`Plane.normal` is the constructor's defaulted parameter).
  A two-class reduction of the same code resolves it, so this is inference
  precision, not a missing inliner case.
- `renderObject`, `getNormalMatrix`, the frustum chain are entered once per
  object from outside any loop of their own.

## What a call costs

Cycle samples (`cycles:u`, no call chains), sample-weighted over app.so
(c8 `entry.py`): prologues (frame, callee-saved saves, argument stores, stack
check, the module-data delta, pooled constants) 0.92M cycles a frame,
epilogues 0.11M, the wrappers' own code 0.36M. That is ~1.4M of 38.3M (3.6%)
for ~0.5M app calls, about 3 cycles each beyond the call and return
themselves. The first number the call count suggests (26 instructions an
entry) overstates it: the entry is stores and register moves that retire in
parallel.

## What was built and measured (parked)

1. **Hot by the call graph.** Guarded inlining copies bodies only into loops
   and into functions "entered from a loop". Built: a `CallTargets` map naming
   the function a call enters as far as the IL says (a direct `call`, a
   resolved method or accessor site, a site's own-callee guesses, and a
   `call.dynamic` of a closure read from the env record its caller closed over,
   resolved through the creating function's `create.func` and `env.set` of
   that slot), and a fixpoint marking every function a loop reaches through
   such calls hot. A self-recursive closure (`projectObject`) is entered from a
   loop by the same rule.
2. **Smallest first outside loops.** In a hot function, after the loops'
   sites in block order (unchanged), the other sites one at a time, smallest
   finished body first, each body's own calls becoming sites a level deeper.
   Without a profile every such site runs once per entry, so the room goes
   furthest on small bodies.
3. **A larger room for those** (3x + 300 instead of 2x + 200): the copy of
   `intersectsObject` in `projectObject` otherwise had no room left for its
   own `intersectsSphere`.

Variants, three_meshes (medians of three rounds; per frame):

| | base | self-recursion only (e1) | call graph (h1) |
|---|---|---|---|
| cycles (M) | 38.86, 38.16, 38.44 | 38.58, 38.47, 37.64 (+0.1%) | 39.74, 38.74, 38.91 (+1.2%) |
| cpu ms | 7.46 | 7.47 | 7.56 |
| instructions (M) | 121.6 | 120.3 (-1.0%) | 119.7 (-1.5%) |
| calls (M) | 1.085 | 1.060 (-2.3%) | 1.041 (-4.1%) |
| taken branches (M) | 7.11 | 7.00 | 6.98 |
| op-cache misses / accesses (M) | 10.50 / 18.54 | 10.34 / 18.15 | 10.58 / 18.32 |
| app.so | 40.3 MB | 40.5 MB | 44.2 MB (+9.6%) |
| `bronze build` | 36.6 s | 36.6 s | 41.3 s (+13%) |
| 90% hot set (IBS) | 148 KB | | 148 KB |

three_skinned, same rounds: base 33.64, 35.40, 34.41M cycles; e1 35.03,
35.23, 33.94 (+1.8%); h1 33.76, 33.92, 34.45 (-1.4%). e1 changes nothing
there but `projectObject`, so ±2% is this scene's noise and neither side
moved. Item 3 on top of h1 measured +1.6% on three_meshes in two rounds.

Why it did not pay, from a cycles profile of h1 against base: `projectObject`
rose 1.05 → 1.73M cycles while the callees it absorbed (`intersectsObject`,
`Sphere.copy`, `Sphere.applyMatrix4`, `Layers.test`, part of `Vector3.copy`)
fell by ~0.6M; `renderObject` rose 0.40M against 0.58M removed from
`getNormalMatrix` and its callees. A copied body executes what the call did,
minus the ~3 cycles of the call: its receiver guards, slot traffic and boxing
stay, because nothing in the caller's context answers them (a guard's way-0
shape is latched at run time, not a fact the copy can use). The hot set did
not shrink (callers' hot lines grew by what the callees' lost:
`projectObject` 88 → 142 lines), so the frontend gained nothing either, and
the copies in cold arms (sprites, lights, LOD, the multi-draw list's `push`
under a class guess that fails for the WebGL render list) are code and build
time for nothing.

Taken out: every variant was within noise or worse, and h1 costs 13% build
time and 10% code. The patch is described here in enough detail to rebuild
it; the parts worth keeping if inlining is revisited are `CallTargets` (the
closure-slot resolution is the only static signal that `projectObject` and
`renderObjects` → `renderObject` are hot) and the smallest-first order.

## What was kept

- **A call between a `+` chain's links is not inlined**
  (`lower/inline_guarded.cpp`). A chain's builder must be read by its next
  link in the same block (`il/verifier.cpp`, `verifyConcatChain`); a body
  copied between `concat.begin` and `concat.end` split the block, and the
  build failed with "internal: guarded method inlining produced invalid IL".
  It needs a resolved method or getter inside a chain in a loop:
  `for (...) out.push(xs[i].hex + ':' + i)` with one class declaring `hex`
  failed to compile on main. Test: oracle `inline_guarded_concat` (resolved
  getters inside chains; receivers of another class, an own method shadowing
  the prototype's, the method and a getter replaced on the prototype, a getter
  throwing inside the chain, recursion inside the chain), through the pin
  matrix. three_meshes' `.text` is byte-identical with and without the fix.
- Diagnostics: `BRONZE_INLINE_GUARDED_TRACE` now also names callers too big
  to take bodies (setProgram, 882 IL); `BRONZE_INLINE_DUMP=<fn>` prints that
  function's IL after inlining.

## Ranked from here

1. **Instructions inside bodies, not calls.** Inlining moves the cost; what
   would cut it is the copy simplifying in its caller: receiver guards the
   caller already made (layout facts reaching into copied bodies), the
   wrapper's argv stores, boxing at the boundary. Until a copy executes less
   than the call did, inlining is code size for nothing.
2. **The property-read stub**: `__bronze_prop_get_ic` is the second hottest
   symbol (1.8M cycles, 0.1M calls a frame, setProgram 17 a mesh). The sites
   are polymorphic reads (materials, uniforms) the static claims leave alone.
3. **Inference precision for field receivers**: `this.normal.dot` and
   `this.center.copy` stay unresolved because a defaulted constructor
   parameter or an escaped owner hides the field's class. Resolving them
   makes them inlinable, but by item 1 that alone buys little.
4. **The wrapper hop** (0.36M cycles plus a second call and return for
   0.1M calls a frame): a tail call from the wrapper (brass has none) or an IC
   entry holding the body's direct entry (an ABI change). Worth ~1% at most.
5. **Prologues** (0.92M cycles): the stack check doubles as the interrupt
   poll and the module-data delta addresses every cell, so neither can be
   dropped from leaves without another mechanism.
