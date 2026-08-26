# Benchmarks

Deterministic benchmark harness and runner for Bronze and the Bro runtime integration.
Measures execution performance across compilation modes (inferred native layouts vs uniform dynamic convention) and compares compiled host apps against interpreted QuickJS.

> [!IMPORTANT]
> **Automated runner hard rule**: `node` is NEVER invoked by the automated runner or test suite (see `CLAUDE.md`).
> Benchmarks measure Bronze against its own history and execution modes. Manual instructions for running outside Bronze (e.g. in Node.js) are documented below for manual reference only.

## Running Benchmarks

Run via PowerShell (Windows native) or Bash:

```powershell
# PowerShell (Windows native):
.\bench\bench.ps1                      # Full suite (5 runs per case, warmup discarded)
.\bench\bench.ps1 -Quick               # Fast iteration (3 runs, infer-only, ~30-45s)
.\bench\bench.ps1 -Filter math         # Filter benchmarks by name/description
.\bench\bench.ps1 -Profile             # Capture top ABI helper calls and IC misses
.\bench\bench.ps1 -Cached              # Reuse compiled binaries if sources unchanged
.\bench\bench.ps1 -PureOnly            # Pure-compute scenes only (no GL/DOM)
```

```bash
# Bash:
bench/run_benchmarks.sh                # Full suite (5 runs per case, warmup discarded)
bench/run_benchmarks.sh --quick        # Fast iteration (3 runs, infer-only, ~30-45s)
bench/run_benchmarks.sh --filter math  # Filter benchmarks by name/description
bench/run_benchmarks.sh --profile      # Capture top ABI helper calls and IC misses
bench/run_benchmarks.sh --cached       # Reuse compiled binaries if sources unchanged
bench/run_benchmarks.sh --pure-only    # Pure-compute scenes only (no GL/DOM)
bench/run_benchmarks.sh --render-only  # Bro WebGL/scenegraph render scenes only
bench/run_benchmarks.sh --json         # Machine-readable JSON-lines only
```

## Benchmark Suite Catalog

### 1. Pure-Compute Scenes (Plain JS, no GL, no DOM)

| Benchmark | Description | Key Subsystems Exercised |
|---|---|---|
| `three_math.js` | Vector3, Matrix4, Euler, Quaternion math loop against vendored Three.js | Math transforms, matrix compositions/inversions, quaternion rotations |
| `object_graph.js` | Graph/tree traversal, search (DFS/BFS), mutation, cloning | Object allocation, depth traversal, property mutations, prototype chains |
| `typed_array_crunch.js` | N-body gravitational physics (Verlet) + Radix-2 FFT | Float64Array / Float32Array numeric operations, math kernels |
| `mesh_churn_2k.js` | 2,000 animated meshes with per-frame matrix world updates | Scene graph hierarchy, Object3D updates, Float32Array geometry churn |
| `instanced_mesh_churn.js` | Three.js InstancedMesh 5,000 instances churn and color updates | InstancedMesh, dynamic matrix/color toArray element writes, math loops |
| `fib.js` | Recursive `fib(30)` | Call overhead on tiny all-`dynamic` vs inferred-f64 function |
| `numeric_loop.js` | 10M-iteration float arithmetic loop | Proven-f64 arithmetic in tight loop |
| `property_access.js` | 1M iterations of `o.a + o.b` | Own-property IC fast path dispatch |
| `repr_slot_kernel.js` | 400k iterations over pinned number fields, inline and out-of-line, with never-written shape-mates beside them | Stage R1 double slots: the store arms' Number test, the collector over a mixed heap. Needs `--pins bench/pins/repr-slot-kernel.pins` to make any slot a double one |
| `repr_flow_kernel.js` | 400k iterations chaining load-arithmetic-store across two objects whose every field is pinned, plus a computed-NaN score in the checksum | Stage R2 dataflow: the raw store and the elided GC root between links, and the NaN frontier a raw store could alias a tag at. Needs `--pins bench/pins/repr-flow-kernel.pins` |
| `proto_dispatch.js` | Depth-3 inherited property read (stable epoch) | Prototype chain IC caching at depth > 0 |
| `proto_dispatch_churn.js` | Depth-3 inherited read with interleaved `new Pt(i)` | Ordinary object property adds vs prototype cache invalidation |
| `typed_array_loop.js` | Float32Array element access vs plain Array | TypedArray buffer access vs JS array indexing |

### 2. Render Scene Benchmarks (Bro Host Execution)

| Scene Benchmark | Mode / Runtime | What It Measures |
|---|---|---|
| `render_scenegraph_host` | Compiled Host (`bro-bronze-host`) | Full Three.js scene graph + WebGL2 context loop (30 frames) |
| `render_wild_orbit_host` | Compiled Host (`bro-bronze-host-wild`) | Unmodified Three.js scene + OrbitControls + textures + lit pixels (30 frames) |
| `render_interpreted_bro` | Interpreted QuickJS (`bro-headless`) | Interpreted 3D scene graph animation under QuickJS engine (30 frames) |

## Manual Node.js Execution Instructions

Each pure-compute benchmark is standard ES module JavaScript (or script) and can be executed unmodified in Node.js for manual, out-of-band comparison.

Because `bench/package.json` specifies `"type": "module"`, run directly from the workspace root or inside `bench/`:

```bash
# From workspace root (Node.js 18+):
node bench/three_math.js
node bench/object_graph.js
node bench/typed_array_crunch.js
node bench/mesh_churn_2k.js
node bench/instanced_mesh_churn.js
node bench/fib.js
node bench/numeric_loop.js
node bench/property_access.js
node bench/proto_dispatch.js
node bench/proto_dispatch_churn.js
node bench/typed_array_loop.js
```

### Reference Node.js Baseline (Node v24.2.0, out-of-band manual reference)

| Benchmark | Node.js Median (ms) | Verified Output / Checksum |
|---|---|---|
| `fib.js` | 41.06 | `832040` |
| `numeric_loop.js` | 65.62 | `60644102826883.61` |
| `property_access.js` | 37.09 | `3000000` |
| `proto_dispatch.js` | 36.13 | `3000000` |
| `proto_dispatch_churn.js` | 38.14 | `3000000` |
| `typed_array_loop.js` | 40.75 | `523826421.8828082 \n 523828354.8980187` |
| `typed_array_crunch.js` | 56.27 | `typed_array_crunch checksum=78849652` |
| `object_graph.js` | 69.38 | `object_graph checksum=-32601148` |
| `three_math.js` | 48.40 | `three_math checksum=405000` |
| `mesh_churn_2k.js` | 92.70 | `mesh_churn_2k checksum=-2112298` |
| `instanced_mesh_churn.js` | 95.28 | `instanced_mesh_churn checksum=1260786` |

## The Benchmark Log

### Campaign summary — stage 3, codegen consumes pins (3.1 → 3.4)

> [!IMPORTANT]
> **The ladder, `bench/mat4_kernel*.js`, ns/call by two-count wall delta over
> 18e6 calls, checksums `400000 / 940000` in every row including node's:**
>
> | | ns/call | what moved |
> |---|---|---|
> | before the campaign | **196.9** | boxed everything |
> | 3.1 pin manifest (`033f4f3`, `c9083b6`) | **27.6** | pinned element form, unguarded f64 arithmetic |
> | 3.2 nullish pins + env-slot typing (`2999b39`) | 27.6 | no move here; this kernel has no nullish and no env slots |
> | 3.3 typed calling convention (`408d13e`) | **25.05** | direct method edge + typed entry + LLVM inlining |
> | 3.4 storage alias families | **23.42** | the guards LLVM was re-testing now fold |
> | E1 direct sibling-closure edges (`9a06bbc`) | 23.55 | no move here; this kernel has no sibling-closure call |
> | E2 ToInt32 inline (`4098379`) | **16.86** | `a.elements[0] = 1.0 + (i & 7) * 0.125` stops calling out per iteration |
> | E3 one root frame per inlined region (`fa24b1f`) | **16.13** | the direct method edge stops carrying the callee's prologue (17.01 for the same session's E2 column) |
> | E4 dead-zone reach widening | 18.09 | a REGRESSION, and not one of the analysis: this kernel's `run` changed PARTITION, so `Matrix4.multiplyMatrices.inl` became a `declare` and stopped inlining. Stage E4's entry has the isolation |
> | E5 cross-partition inlining (`llvm_partition.cpp`) | **16.43** | the callee is kept in the caller's partition as `available_externally`, so the split stops eating stage 3.3's inline. Recovers all of E4's regression and 0.57 ns more; the same session's E3 column reads 17.00 |
> | node v24.2.0, same method | **14.20** | re-measured; Stage 3.3 quoted 15.7 on a busier box (14.23 again in E2's session, 14.31 in E3's, 14.39 in E4's, 14.83 in E5's) |
>
> **12.2× against where the campaign started and 1.14× off node on this
> kernel.** The pre-registered ≤ 20 ns target IS reached, at stage E2, and NOT
> by the mechanism it was registered against: stage 3.4 was right that the
> guards remaining on this kernel are worth about **1 ns in total** and wrong
> to conclude from it that the target was unreachable. What stood between 23.4
> and 16.9 was one helper call per iteration — `bronze_to_int32_f64` for the
> `i & 7` that perturbs the input — costing 6.5 ns for an operation that is
> two machine instructions, most of it as an optimization barrier rather than
> as the call. The lesson the campaign should carry: an opaque external call
> in a loop is not priced by its own cost.
>
> **Per fixture, default against `--pins bench/pins/threejs-math.pins`, one
> interleaved session, raw wall medians of 9 (a bronze exe's process floor on
> this box is 5.62 ms, node's is 32.34 ms — subtract the right one before
> comparing across engines; never compare these absolutes to an earlier
> entry's, which carry a different floor):**
>
> | fixture | default | `--pins` | delta | node |
> |---|---:|---:|---:|---:|
> | `three_math` | 41.65 | **21.86** | −19.80 | 50.07 |
> | `object_graph` | 46.71 | 47.42 | +0.72 | 69.88 |
> | `typed_array_crunch` | 53.13 | 51.99 | −1.14 | 56.04 |
> | `mesh_churn_2k` | 74.42 | 72.37 | −2.05 | 89.27 |
> | `instanced_mesh_churn` | 110.73 | 104.62 | −6.10 | 95.82 |
> | `fib` | 10.97 | 10.12 | −0.86 | 41.35 |
> | `numeric_loop` | 35.14 | 34.56 | −0.58 | 65.03 |
> | `property_access` | 11.43 | 10.41 | −1.01 | 37.49 |
> | `proto_dispatch` | 21.84 | 20.83 | −1.00 | 37.07 |
> | `proto_dispatch_churn` | 53.19 | 54.33 | +1.14 | 40.18 |
> | `typed_array_loop` | 29.61 | 28.57 | −1.05 | 42.47 |
>
> All eleven produce output **byte-identical** under default, under `--pins`,
> and under node (`cmp` on 33 captured stdouts, 22 comparisons, no diff). A manifest that
> names nothing a fixture uses still costs it nothing measurable; `object_graph`
> and `proto_dispatch_churn` are the two that read +1 ms, which is the run-to-run
> width on those two, not a regression the IL shows.
>
> **Kernels, three columns, same session:**
>
> | kernel | default | `--pins` | node v24.2.0 | checksums |
> |---|---:|---:|---:|---|
> | `mat4_kernel` | 164.43 | **23.42** | 14.20 | `400000 / 940000` |
> | `env_slot_kernel` | 58.47 | **56.70** | 5.05 | `126000020` |
> | `nullish_pin_kernel` | 37.72 | **12.77** | 5.32 | `825756/700159/NaN/-563350` |
> | `call_chain_kernel` chained/flat | 23.75 / 20.50 (ratio 1.16) | **20.00 / 18.25 (ratio 1.08)** | 1.88 / 1.88 (1.00) | `296000000 / 296000000` |
>
> Reproduce. Every kernel number above is a two-count wall delta — the big
> fixture minus its small twin, which cancels process startup exactly — taken
> INTERLEAVED, one run of every column per round, medians across 13 rounds, so
> a drift in machine state lands on every column equally:
> ```
> bronze build bench/mat4_kernel.js        -o mat4_pin_b.exe --pins bench/pins/threejs-math.pins
> bronze build bench/mat4_kernel_small.js  -o mat4_pin_s.exe --pins bench/pins/threejs-math.pins
> # ns/call = (median(mat4_pin_b) - median(mat4_pin_s)) * 1e6 / 18000000
> ```
> `env_slot_kernel` and `nullish_pin_kernel` have no committed small twin; the
> second count is the same file with `ITERS` divided by ten (6000000 → 600000
> and 8000000 → 800000), so the deltas are 5.4e6 iterations and 4 × 7.2e6
> steps. `call_chain_kernel` times its two loops IN PROCESS and is quoted raw.
> The automated runner never invokes node; the node column above is a manual
> out-of-band run, `node bench/<fixture>.js`, and is here for the checksum
> first and the millisecond second.

### Campaign summary — stage E, closures and environment records (E1 → E5)

> [!IMPORTANT]
> **THE CANONICAL LADDER. One idle session, ONE compiler binary, the campaign's
> seams peeled in campaign order** (`bash bench/tools/ladder.sh <bronze.exe>
> <dir>`, then `bench/tools/ladder_specs.py` and `interleave.py`). Every stage
> of this campaign shipped an A/B seam and every seam is read once per
> invocation, so `BRONZE_NO_CLOSURE_EDGE` + `BRONZE_NO_INLINE_TOINT32` +
> `BRONZE_NO_PURE_CONVERSIONS` + `BRONZE_NO_ENV_TRIPWIRE` +
> `BRONZE_NO_FRAME_MERGE` + `BRONZE_NO_DEFINITE_INIT` + `BRONZE_NO_PURE_PREDICATES`
> + `BRONZE_NO_CLOSURE_PARAM_PROOF` + `BRONZE_NO_DEFINITE_REACH` +
> `BRONZE_NO_XPART_INLINE` all on IS the stage 3.4 compiler, and dropping them
> a stage at a time walks forward to today. This exists because stages E2 and
> E3 both documented cross-session drift of ±50 % on the millisecond fixtures
> with identical IL: a six-stage table assembled from six sessions is not a
> ladder, it is six ladders. The manifest is the COMMITTED one in every row —
> peeling seams is a statement about the compiler, and changing the manifest
> under it would be a statement about the manifest.
>
> **Kernels, 101 rounds, two-count wall delta; node re-measured out of band in
> the same session. Checksums identical in every cell of every row.**
>
> | | stage 3.4 | E1 | E2 | E3 | E4 | E5 | node v24.2.0 |
> |---|---:|---:|---:|---:|---:|---:|---:|
> | `env_slot_kernel` ns/iter | 56.62 | 47.38 | 15.12 | 10.86 | 10.92 | **10.90** | 5.35 |
> | `mat4_kernel` ns/call | 24.50 | 23.98 | 17.34 | 17.00 | 18.31 | **16.43** | 14.83 |
> | `nullish_pin_kernel` ns/step | 13.09 | 13.10 | 11.54 | **11.40** | 11.41 | 11.42 | 5.59 |
> | `call_chain_kernel` chained / flat | 20.50 / 19.00 | 20.50 / 19.00 | 11.63 / 12.88 | 9.63 / 10.00 | 9.63 / 8.88 | **9.63 / 8.75** | 2.00 / 1.88 |
>
> **Millisecond fixtures, 51 rounds, raw wall medians** (a bronze exe's process
> floor and node's differ by ~27 ms on this box; never compare the absolutes
> across engines, only within a row):
>
> | | stage 3.4 | E1 | E2 | E3 | E4 | E5 | node |
> |---|---:|---:|---:|---:|---:|---:|---:|
> | `typed_array_crunch` | 54.70 | 54.63 | 49.35 | 49.45 | **37.74** | 37.91 | 59.06 |
> | `three_math` | 43.86 | 45.92 | **43.04** | 45.80 | 45.67 | 44.64 | 52.49 |
> | `mesh_churn_2k` | 77.44 | 79.05 | 78.73 | 75.56 | **74.13** | 75.31 | 96.53 |
> | `object_graph` | 48.81 | 48.90 | **48.46** | 49.09 | 48.44 | 48.95 | 74.62 |
>
> Checksums `126000020 / 12600020`, `400000 / 940000`, `825756/700159/NaN/-563350`,
> `296000000`, `78849652`, `405000`, `-2112298`, `-32601148` — every cell, every
> column, and node's.
>
> **What the ladder shows that no single stage's entry could.** The campaign is
> **5.2×** on `env_slot_kernel` and **2.1×** on `call_chain_kernel`'s chained
> arm; it is **1.49×** on `mat4_kernel` and **1.44×** on `typed_array_crunch`,
> and it is worth NOTHING on `object_graph`, which is the honest shape of a
> campaign aimed at closures and environment records. The one cell that still
> contradicts a published stage number is E1's `env_slot`, which reads 47.38
> where E1 published 50.82 — the seam-off compiler is TODAY's compiler with
> E1's mechanism disabled, so it carries every later stage's work that E1's
> binary did not, and a peeled ladder is a clean set of DIFFERENCES and only
> approximately a set of historical absolutes. The other one is now closed:
> E4's `mat4` column was a 1.9 ns regression against E3 caused by the emission
> split un-inlining a callee, and stage E5's mechanism takes that column past
> where E3 stood. This session runs ~1 % slower than E4's throughout (E3's
> `mat4` reads 17.00 here against 16.25 published), so read DOWN a column, not
> across sessions.
>
> **`ladder.sh` now peels a seventh column, `b1`** (stage B1's write barriers,
> `BRONZE_NO_PIN_BARRIERS=1`), which is the INNERMOST seam and is therefore on
> in all six columns above — none of the compilers they reproduce emitted a
> barrier. The table above is E5's session and is left as it was measured; the
> seven-column ladder from B1's own session is in the Stage B1 entry below,
> because mixing two sessions in one table is the thing this block exists to
> prevent.

- **Stage R2 (the compiler spends the representation: raw stores, and numbers stop being rooted)** — 2026-08-26:
  > [!NOTE]
  > **R1 gave a shape the ability to say a slot's eight bytes are a double and
  > deliberately spent none of it. R2 is the compile-time half: a fact about an
  > IL VALUE (`src/codegen-llvm/llvm_repr.{h,cpp}`), spent at the places R1 had
  > to leave a test.** Two things land, and the second is the larger: a store
  > whose value is a proven Number emits **no representation test at all**, and
  > a `dynamic` value proven never to be a heap pointer gets **no GC root** —
  > which removes its root store and the reload at every use. Full design:
  > [docs/slot-representation.md](../docs/slot-representation.md).
  >
  > **Why the raw store needs no guard of its own.** A Number's box IS the
  > canonical double a double slot promises, so the same eight bytes are correct
  > for a double slot and for a boxed one alike. The store is right whichever
  > way `double_slots` reads, which makes it sound across a **generalization
  > between two visits to the same site** — stronger than a guard, not weaker.
  > What is still required is the dominating shape guard R1 already required;
  > R2 widened no site's guard coverage.
  >
  > **Seam: `BRONZE_NO_REPR_CODEGEN=1`, read by the COMPILER** — every value
  > comes back `Unknown`, every site emits the stage R1 sequence, every value
  > keeps its root. Build-time and not run-time on purpose: what it isolates is
  > the emitted code. It is measured below against the R1 TREE itself, and the
  > two agree to 0.3%. `BRONZE_REPR_CODEGEN_STATS=1` prints the static site
  > counts.
  >
  > **The headline, three-way, one interleaved session** (61 rounds after a
  > discarded warmup, order alternated per round so no arm holds first position,
  > raw wall of the whole process in ms). `R1 tree` is `6734cd1` rebuilt:
  >
  > | kernel | R1 tree | R2 seam-off | R2 | checksum |
  > |---|---:|---:|---:|---|
  > | `three_math` (`--pins bench/pins/threejs-math.pins`) | 22.92 | 22.99 | **21.36** | `405000` |
  > | `repr_slot_kernel` (`--pins bench/pins/repr-slot-kernel.pins`) | 16.36 | 16.34 | **15.36** | `180008650039037` |
  >
  > Means: 22.97 / 23.07 / 21.71 and 17.33 / 17.56 / 15.46. **`three_math`'s R1
  > column reproduces stage R1's published 22.9 ms exactly**, which is what
  > licenses reading the rest of the row. R2 is **6.8%** and **6.1%** under it.
  > Net of the ~6.3 ms every one of these processes spends starting up (an empty
  > compiled program, measured the same way), the compute halves move **9.4%**
  > and **9.9%**.
  >
  > **The seam-off column is the R1 column.** 22.99 against 22.92, and 16.34
  > against 16.36 — and `tm_seam.exe` and `tm_r1.exe` are byte-for-byte the same
  > SIZE (4,733,952), differing only in the link timestamp. That is the property
  > a compile-time seam is for, checked rather than asserted.
  >
  > **The five-kernel sweep** (same harness, 61 rounds, kernels run two and four
  > at a time so the medians are not competing with six other processes):
  >
  > | kernel | R2 | seam-off | Δ | checksum |
  > |---|---:|---:|---:|---|
  > | `three_math` | 20.36 | 21.91 | **-7.1%** | `405000` |
  > | `repr_slot_kernel` | 14.49 | 15.22 | **-4.8%** | `180008650039037` |
  > | `repr_flow_kernel` (`--pins bench/pins/repr-flow-kernel.pins`) | 11.09 | 11.39 | -2.6% | `131610905018/127` |
  > | `property_access` (no pins) | 10.18 | 10.71 | -4.9% | `3000000` |
  > | `object_graph` (no pins) | 47.96 | 48.15 | -0.4% | `-32601148` |
  >
  > **Every checksum is bit-identical in every cell of both tables**, which is
  > the acceptance condition: a speedup with a changed checksum is a miscompile.
  > The two kernels with no pinned field are flat, which is the zero-tax
  > condition read directly — `object_graph` at -0.4% is inside its own noise
  > either way.
  >
  > **`bench/repr_flow_kernel.js` is new**, and it is the kernel R1 did not
  > need: every line of its loop is a load out of a pinned slot, arithmetic, and
  > a store into another one, with two objects alternating so a real store site
  > sits between every pair of links. Its checksum's second field is a
  > seven-reader score over a COMPUTED NaN pushed through the same slots, which
  > is the frontier a raw store could silently alias a tag at.
  >
  > **Static counts, from `BRONZE_REPR_CODEGEN_STATS=1`.** `three_math`: 252
  > functions planned, 1,042 values proven Number, **1,264 roots elided**, **604
  > raw store sites**, 0 `sitofp` stores. `repr_slot_kernel`: 4 / 58 / 63 / 44 /
  > 0. `object_graph`, with no manifest at all: 11 / 75 / 106 / 24 / 0. The
  > counter exists because every R2 arm is conditional on a proof — a stage that
  > proves nothing emits the code it replaced and the whole suite still passes,
  > so the site count is the only thing separating "the fast arm is correct"
  > from "the fast arm is ever taken".
  >
  > **Two things were built, measured, and then removed or not built.** (a) The
  > raw LOAD: because R1 canonicalizes, a double slot's eight bytes ARE the
  > boxed Number, so a raw load is bit-identical to the load already emitted —
  > and `three_math` has **zero** checked `unbox.f64` whose operand is a
  > static-slot `prop.get`, so a get-side representation flag would have had
  > nothing to say. R3 input. (b) An emitter arm for `dynamic` arithmetic over
  > two proven Numbers, which the counter said has **zero sites** in every
  > kernel and in a probe built to provoke it — `lower_infer` already types
  > those chains `f64`, so the unboxed dataflow R2 was to add is the typing that
  > was already there. Removed rather than shipped.
  >
  > **Caveats.** (a) `sitofpStores=0` everywhere: `emitToInt32`'s I32 result is
  > converted straight back to F64 by every bitwise operator, so `box.i32`
  > reaches a property store nowhere in the corpus and the R1 gap this arm was
  > written for does not exist in today's lowering. The arm is kept and pinned
  > by unit tests against hand-built IL, and it is dead. (b) The box this was
  > measured on carries occasional multi-millisecond outliers; every table above
  > is a MEDIAN with the mean beside it, and the two agree wherever the sd is
  > under 1 ms. (c) `property_access`'s -4.9% is a 0.5 ms move on a kernel with
  > 2 raw stores and 4 elided roots, and it read +3-6% the other way in a
  > six-variant session — treat it as flat, not as a win.

- **Stage R1 (per-slot representation: a shape can say a slot is an f64)** — 2026-08-26:
  > [!NOTE]
  > **A property slot held one thing — a NaN-boxed `Value` — and that
  > uniformity is what makes three.js's `Vector3`/`Matrix4`/`Quaternion` pay a
  > tag at every property boundary. A `Shape` can now say, per slot, that the
  > slot's eight bytes ARE a double.** What R1 lands is the STORAGE MODEL, the
  > machinery that keeps the claim true against every store path, and the
  > census that says which other slots are worth claiming. Generated code does
  > not spend the promise yet — no raw `f64` load, no elided tag test; that is
  > stage R2. **So this entry publishes no speedup, and the number that matters
  > is the absence of one in either direction.** Full design:
  > [docs/slot-representation.md](../docs/slot-representation.md).
  >
  > **Mechanism.** A node is born `SlotRepr::Double` when the store that
  > creates the property is a Number *and* the name is on the eligibility list
  > the module registers at init (`bronze_register_slot_repr`), which the
  > lowerer builds from the `--pins` manifest's `number` fields on classes
  > whose layout inference proved. bronze has no deopt, so the claim is taken
  > back by a SHAPE CHANGE: a non-Number reaching a double slot moves that
  > object to a shape rebuilt with the slot boxed, leaving every shape-mate
  > untouched. Every runtime write goes through `ObjectHeader::setSlot`;
  > generated code's three bare stores (the set-site IC's own arm, its
  > transition arm, and the static-slot site in both its identity and family
  > forms) test the value for Number inline and miss to the helper when it
  > fails — once per field, not once per store, because the miss generalizes
  > the slot and the refilled entry has nothing left to test.
  >
  > **Seam: `BRONZE_NO_SLOT_REPR=1`** — no node is ever created double, every
  > `double_slots` word stays zero, storage is exactly what it was.
  > `BRONZE_SLOT_REPR_STATS=1` prints the creation counters,
  > `BRONZE_SLOT_REPR_CENSUS=1` adds per-(shape, slot) stability and the
  > boxed-slot candidate set R2 reads, `BRONZE_SLOT_REPR_OBSERVED=1` makes
  > every key eligible (implemented, off by default).
  >
  > **Four kernels, one interleaved A/B/A/B session** (each variant's exe built
  > by the bronze of its own commit; 9 rounds after a discarded warmup, raw
  > wall of the whole process, means in ms). `baseline` is `abde46b`, the
  > commit before the stage:
  >
  > | kernel | baseline | R1 seam-on | R1 seam-off | checksum |
  > |---|---:|---:|---:|---|
  > | `three_math` (`--pins bench/pins/threejs-math.pins`) | 22.4 | 22.9 | 22.9 | `405000` |
  > | `repr_slot_kernel` (`--pins bench/pins/repr-slot-kernel.pins`) | 15.9 | 16.1 | 16.0 | `180008650039037` |
  > | `property_access` (no pins) | 10.7 | 10.6 | 10.7 | `3000000` |
  > | `object_graph` (no pins) | 48.8 | 49.0 | 48.3 | `-32601148` |
  >
  > Measured at `613105b`; the three commits after it change comments, this
  > entry, and a census-only path. A re-run on the final tree landed on a
  > noisier box (sd 5–8 ms against 0.2–0.4 above) and reproduces the table by
  > MEDIAN in every cell — 23.2 / 23.3 / 23.5, 16.1 / 16.7 / 16.3, 11.3 / 11.3
  > / 11.3, 47.7 / 48.7 / 48.7 — which is the reading to trust when a session
  > carries outliers.
  >
  > **Every checksum is bit-identical in every cell**, which is the acceptance
  > condition this stage was given: a speedup with a changed checksum would be
  > a miscompile. The largest movement is `three_math`'s +0.5 ms, and it is the
  > same seam-on and seam-off — it is the store-side guard instructions, which
  > are emitted whether or not any slot is ever a double, not the cost of the
  > representation. The two kernels with no pinned field are flat, which is the
  > stage's zero-tax condition read directly.
  >
  > **The mechanism is demonstrably live.** `three_math` with the manifest
  > creates **10 double slots** — `Vector3.x/y/z`, `Quaternion._x/_y/_z/_w`,
  > `Euler._x/_y/_z` — over 338 shape nodes, with **zero generalizations**: the
  > manifest's promises hold, which a census run CHECKS rather than assumes.
  > `repr_slot_kernel` creates 11 over 89. Both run clean under
  > `BRONZE_HEAP_VERIFY=1`, whose `verify_space` now asserts the R1 invariant
  > itself — every slot a shape calls a double holds a Number — at every
  > collection.
  >
  > **`helper stores` reads near zero and that is the good news.** The counter
  > counts stores that reached `setSlot`; `three_math` reports 40 of its
  > ~660,000, because the other 659,960 took an inline arm, tested the value
  > and stored it themselves. An earlier cut of this stage refused those arms
  > outright rather than teaching them the test, and the same two kernels read
  > **51.6 ms and 183.9 ms** — 2.3× and 11.7× against their own seam-off runs.
  > That number is kept here because it is the measurement that chose the
  > design: a representation the store paths can only respect by giving up
  > their fast paths is not one worth having.
  >
  > **Caveats.** (a) Nothing here is a win and nothing here should be read as
  > one; the win is R2's to publish or fail to. (b) The collector's precision —
  > skipping double slots, scanning the out-of-line block from the owner — is a
  > provable no-op today, because a canonicalized double slot's word is also
  > the number's box; it is landed now so R2 is a codegen change rather than a
  > collector change. (c) `BRONZE_SLOT_REPR_OBSERVED=1` is shipped but off:
  > under it a field that alternates re-splits the shape tree on every
  > constructor, because the transition arm's cached node outlives the sticky
  > demotion mark. Pinned fields do not alternate, which is the whole
  > difference between a promise and an observation. (d) An `Int32`-tagged
  > store — what an `il::Type::I32` boxes into — misses the arms every time,
  > because its bits are a tag and a payload rather than an f64; a pinned field
  > written as `this.n = i | 0` therefore pays a helper call per store. None of
  > the four kernels writes one (`three_math` reaches the helper 40 times in
  > ~660,000 stores), so nothing above measures it.

- **Stage C1 (census: the manifest writes itself)** — 2026-08-25:
  > [!NOTE]
  > **A `--pins` manifest was hand-written. `bronze build --census <out.pins>`
  > now writes one from a run of the program.** On the four pinned kernels the
  > file it writes reproduces what the hand-written file bought
  > (`env_slot_kernel` **10.60** vs 10.62 ns/iter, `nullish_pin_kernel` 11.24 vs
  > 11.12 ns/step, `call_chain_kernel` 9.63/8.75 vs 9.50/8.75 ns, `mat4_kernel`
  > 16.53 vs 15.98 — the one gap is isolated and explained below). On
  > `three_math`, the fixture nobody ever wrote a manifest for, it **BEATS the
  > hand-written one**: 40.41 ms unpinned, 21.40 with `bench/pins/threejs-math.pins`,
  > **18.47 with the census's file**, and the whole of that last 2.9 ms is 23
  > `param` entries no hand author bothered to write. Checksums identical in
  > every cell of every table below. The loop is OFFLINE and there is no JIT
  > anywhere in it: two compiles, one artefact, and the artefact is an ordinary
  > text file a person can read, diff and commit.
  >
  > ```sh
  > bronze build app.js -o census.exe --census app.pins   # 1. instrument
  > ./census.exe                                          # 2. a representative run
  > bronze build app.js -o app.exe --pins app.pins        # 3. the real build
  > ```
  >
  > **The mechanism, and its one structural commitment.** A census SITE exists
  > only where lowering ran out of static answers. That is E4's HANDOFF (c)
  > written as code rather than as a filter: `planClosureParamNumbers`, the
  > env-slot fixpoint and the signature join all run BEFORE any site is created,
  > and every one of them removes sites. A parameter the proof typed `f64` has
  > no manifest line to write, so it gets none — the overlap HANDOFF (c) warned
  > would inflate a census's precision cannot be double-counted here because it
  > never enters the count.
  >
  > | site | recorded at | the entry it supports |
  > |---|---|---|
  > | env slot | every store to a captured binding the fixpoint could not type | `function <fn>.<binding>: number` |
  > | parameter | **the callee's entry** | `param <owner>(<p>): number` |
  > | return | the `return` statement | `return <owner>: number` |
  > | field | the same six store paths B1's barrier sits on | `<Class>.<field>: <kind>` |
  > | opaque store | a store through a receiver inference types `dynamic` | *nothing* — it marks `@observed` |
  >
  > **Parameters at the CALLEE'S ENTRY, and that is the whole reason the
  > instrument exists.** E4's proof enumerates the call sites of a nested
  > declaration whose name never leaves callee position; the closure a factory
  > HANDS OUT has call sites that enumeration provably cannot reach. The entry
  > sees every call there is. On `env_slot_kernel` the census recovers
  > `param render(iters): number` — the one line of the eleven E4 named as the
  > case a manifest is still for — with no manifest and no annotation, and it
  > proposes NOTHING for `stateChanges`/`frames` (fixpoint-proved) or for
  > `setBlending`/`useProgram`/`setDepthFunc`'s parameters (E4-proved).
  >
  > The site table is handed over WHOLE at module init
  > (`bronze_census_register`), not accumulated as sites are hit, so "never
  > observed on this run" is a fact the file can state and a STATIC refusal
  > works on a run that never reaches the code. Aggregation is a JOIN and never
  > a vote: all sites Number is a pin, one site anything else is no pin, and no
  > threshold changes that — a pin is spent *unchecked* at the read, so "almost
  > always a number" is not a weaker version of the claim, it is a different and
  > false one.
  >
  > **Two strengths, because B1's enforcement is not total.** A store through a
  > receiver inference types `dynamic` gets no barrier while a class-known read
  > elsewhere still spends the claim (B1's negative 1). The census reports that
  > gap per FIELD NAME, which is the granularity the manifest itself has:
  >
  > ```
  > Vector3.z: number              # every store to it is from a site the compiler can type
  > Vector3.x: number @observed    # some store to a field named 'x' is not
  > ```
  >
  > `@observed` is a **hard error in a default build**, by name, with the line
  > quoted; `--pins-allow-observed` accepts it, and accepting it is the
  > deliberate act of taking back B1's guarantee for one entry named in a file.
  > The `infobs` columns below are the A/B for what refusing them costs, and on
  > these fixtures it is nothing.
  >
  > **Four refusals, each a real shape the census met.** A return whose body can
  > fall off its end (statically refused — reachability is a property of the
  > program and no run can be asked about it). An owner spelling that would
  > govern two IL functions (a `--pins` entry matches by suffix, and the second
  > owner may have a defaulted parameter, which is a BUILD FAILURE rather than a
  > wrong number). A field observed only ever nullish (widening it to
  > `number-or-nullish` buys nothing — the pin licenses the coercing position on
  > the *number* arm — and risks everything, because "not assigned yet" is what
  > an optional field looks like on a short run and an object is what it holds
  > on a long one; `Material.clippingPlanes` is exactly that). And a target no
  > manifest line can spell, which is the second defect below.
  >
  > **EMITTED vs HAND, every fixture, every divergence explained.** The `inf`
  > column is the census file with `@observed` stripped — what a DEFAULT build
  > accepts.
  >
  > | fixture | hand | emitted | `@observed` | sites | divergence |
  > |---|---:|---:|---:|---:|---|
  > | `env_slot_kernel` | 12 | 9 | 0 | 13 | −5 `param`, +2 `return` |
  > | `mat4_kernel` | 14 | 25 | 3 | 476 | −6 fields, +20 `param`, +1 `return` |
  > | `nullish_pin_kernel` | 4 | 7 | 0 | 11 | **exact superset**: +1 `param`, +2 `return` |
  > | `call_chain_kernel` | 25 | 27 | 0 | 28 | **exact superset**: +2 `return` |
  > | `three_math` | 14 (borrowed) | 34 | 3 | 519 | −3 fields, +23 `param` |
  > | `tests/oracle/threejs/main.js` | none | 101 | 20 | 1718 | — |
  >
  > Every divergence is one of three things and none of them is a miss:
  >
  > * **The five missing `param` lines on `env_slot_kernel` are the five E4's
  >   proof made redundant.** The committed manifest says so itself — the module
  >   built without them is byte-identical LLVM IR — and they stay there only
  >   because the campaign ladder's pre-E4 rows need them. The census cannot
  >   propose them because there is no site. HANDOFF (c)'s overlap, visible as
  >   an absence rather than as a correction.
  > * **The missing FIELD entries are classes the run never touched.**
  >   `mat4_kernel` never constructs a `Vector2` or a `Matrix3` and never sets an
  >   `Euler`; `three_math` is missing exactly `Matrix3.elements` and
  >   `Vector2.x/y` for the same reason. The hand manifest is written against
  >   the LIBRARY, the census against the RUN. It costs nothing here — code that
  >   never runs is on no hot path, and `fieldsonly` below is 20.44 ms against
  >   the hand manifest's 21.40 — but it is the sharpest single statement of
  >   what a census is and is not.
  > * **The extra entries are `param` and `return` lines a hand author never
  >   bothered with.** They are what wins `three_math` and they are what found
  >   the first defect below.
  >
  > `mat4_kernel`'s `Vector3.x/y/z` is the cell worth staring at: the census
  > emits them `@observed` and the hand manifest makes the same claim unmarked.
  > **The census is right and the hand manifest is optimistic.**
  > `Matrix4.decompose` writes `position.x = te[12]` and `scale.x = sx` through
  > receivers inference types `dynamic`, so those stores carry no barrier while
  > every `Vector3`-typed read still spends the claim. That is B1's negative 1
  > alive in real three.js, found by an instrument rather than by a reader.
  >
  > **TWO COMPILER DEFECTS THE CENSUS FOUND**, and finding them is the argument
  > for the instrument. Neither is a census bug; both are shapes a hand author
  > had never written down and so had never tripped.
  >
  > 1. **A pinned RETURN cost the same function its parameter proof**
  >    (`0da1529`). `flow_expr.cpp`'s call rule answers a pinned
  >    `return <owner>: number` early, and answering it by RETURNING skipped the
  >    loop just below that joins the site's argument types into the callee's
  >    `observedParams`. So `return run: number` — an entry no hand manifest in
  >    the tree contains, which the census proposes because lowering left `run`'s
  >    return `Dynamic` — silently un-typed `run`'s own parameter:
  >    `func run(%0: f64) -> dynamic` became `func run(%0: dynamic) -> f64` and
  >    the hot loop's `i < iters` went from an `fcmp` to a `box.f64` plus
  >    `rel.lt`. **1.82 ns/call on `mat4_kernel`**, isolated by bisecting the
  >    manifest one line at a time after partition placement was checked first
  >    and cleared. The pin is noted instead of returned and still wins over the
  >    join for the RESULT. A pin adds a promise; it must never subtract a
  >    proof. With the fix the inferred manifest gives `func run(%0: f64) -> f64`,
  >    strictly better than the hand manifest's `f64 -> dynamic`.
  > 2. **A target no manifest line can spell was emitted anyway** (`d2d1650`).
  >    A class accessor lowers to an IL function named `Euler.set x` — a space in
  >    the middle — so a census of the three.js oracle proposed
  >    `param Euler.set x(value): number` and the build handed that file refused
  >    to parse it. Getters, computed method names and quoted field keys arrive
  >    the same way. This is the census's worst failure mode and the only one it
  >    has: not a wrong claim, which B1 turns into a `TypeError`, but a file the
  >    next build cannot read. The manifest's identifier grammar is now applied
  >    where a site is created; **nine of the oracle's 110 proposals** were of
  >    this kind, and both defects have a regression test.
  >
  > **THE ROUND TRIP, one idle session, ONE compiler binary**
  > (`bash bench/tools/census_ab.sh <bronze.exe> <dir>`, then `interleave.py`;
  > `selftimed.py` for `call_chain`). Every `inf` column was censused minutes
  > before the build that consumed it. Kernels at 101 rounds by two-count wall
  > delta, millisecond fixtures at 51 rounds raw.
  >
  > | | hand | **inferred** | inferred, `@observed` accepted |
  > |---|---:|---:|---:|
  > | `env_slot_kernel` ns/iter | 10.62 | **10.60** | — |
  > | `mat4_kernel` ns/call | **15.98** | 16.53 | 16.53 |
  > | `nullish_pin_kernel` ns/step | **11.12** | 11.24 | — |
  > | `call_chain_kernel` chained / flat | **9.50 / 8.75** | 9.63 / 8.75 | — |
  >
  > Checksums `126000020 / 12600020`, `400000 / 940000`,
  > `825756/700159/NaN/-563350`, `296000000 / 296000000` — every cell.
  >
  > **`mat4_kernel` is the one cell that does not reproduce, and it bisects
  > cleanly** (same session, 101 rounds, same checksums):
  >
  > | manifest | ns/call |
  > |---|---:|
  > | hand | **15.96** |
  > | inferred | 16.58 |
  > | inferred − `return run: number` | 16.43 |
  > | inferred − the 20 `param Matrix4.set(nXX)` | 16.10 |
  >
  > So 0.48 of the 0.62 ns is the twenty `Matrix4.set` parameter pins, and the
  > cause is not the analysis. `a.set(...)` is called TWICE, outside the loop.
  > Pinning its sixteen parameters gives it a typed entry, and LLVM then inlines
  > `Matrix4.set.inl` into `run` — the post-optimization body of `run` goes from
  > 3827 lines with two `call ...Matrix4.set.inl` to 4609 lines with none, +94
  > loads and +24 stores, with `fmul` and `fadd` unchanged at 65 and 51.
  > Partition placement was checked FIRST, as E4/E5 require, and cleared: two
  > partitions in every column and E5's `available_externally`
  > `multiplyMatrices.inl` present in the partition holding `run` in all of
  > them. **A sound pin made a cold sixteen-argument setter cheap enough to
  > inline into the hot function.** That is the inliner's cost model, not the
  > manifest's soundness, and the census's own output is what makes it legible:
  > the entry reads `# 2 obs / 1 site` on a twenty-million-iteration run, which
  > is a line a reader can delete.
  >
  > **`three_math`, the fixture with no hand manifest, and the stage's real
  > result** (51 rounds, raw ms, checksum `405000` in every cell, reproduced
  > three times to within 0.13 ms):
  >
  > | manifest | ms | vs unpinned |
  > |---|---:|---:|
  > | none | 40.41 | — |
  > | hand `bench/pins/threejs-math.pins` | 21.40 | 1.89× |
  > | census, fields only | 20.44 | 1.98× |
  > | hand fields + census `param` entries | 18.40 | 2.20× |
  > | **census (`inf`, default-safe)** | **18.47** | **2.19×** |
  > | census, `@observed` accepted | 18.46 | 2.19× |
  >
  > The campaign ladder builds `three_math` with NO manifest, which is why its
  > published column has read ~41-45 ms for five stages: **the fixture was
  > leaving 2.2× on the table and nobody had written the file.** The census
  > wrote it in one run. The attribution row is the point — hand fields plus the
  > census's `param` entries is 18.40 and the census's own file is 18.47, so
  > essentially all of the 2.9 ms over the hand manifest is twenty-three
  > `param <Class>.<method>(<p>): number` lines, and the census's smaller field
  > set costs nothing.
  >
  > **THE ENFORCEMENT INTERLOCK, end to end.** Census a program that only ever
  > passes numbers; then run a program that passes a string:
  >
  > ```
  > $ ./base_census.exe && grep param lock.pins
  > checksum 7
  > param render(n): number  # 1 obs / 1 site
  > $ ./violate.exe                      # built with --pins lock.pins
  > slot true pin 'param render(n): number' violated: the value is a string
  > alive 3
  > ```
  >
  > `true` is `e instanceof TypeError`, the message names a LINE OF THE FILE THE
  > CENSUS WROTE, and `alive 3` is the same program continuing afterwards. That
  > is the precision/recall argument made concrete: the census may be wrong
  > about a path it did not see, and being wrong is a diagnostic that points at
  > the line to delete.
  >
  > **HONEST NEGATIVES.**
  >
  > * `tests/oracle/threejs/main.js` — the biggest real program in the tree —
  >   censuses to 101 entries over 1718 sites and builds and runs
  >   **byte-identical to `main.expected`** in all three columns, but it is not
  >   a benchmark: the whole program is ~9 ms of wall, essentially all process
  >   start. Measured anyway and quoted as the noise it is: none **9.02**, inf
  >   **9.02**, infobs **8.99** ms. `three_math` is the real answer to "a
  >   fixture nobody wrote a manifest for".
  > * Refusing `@observed` costs nothing measurable anywhere here (`inf` and
  >   `infobs` agree within the harness's width on both `mat4` and `three_math`).
  >   That is a statement about these fixtures, not about the mechanism: the
  >   entries refused on `mat4` are `Vector3.x/y/z` and this kernel's hot loop
  >   is `Matrix4` arithmetic.
  > * `mat4_kernel` and `nullish_pin_kernel` are both slightly SLOWER on the
  >   inferred manifest (+0.55 ns/call, +0.12 ns/step). The first is explained
  >   above; the second is at the edge of the harness's width and is not
  >   explained further, because chasing 0.12 ns would mean quoting a number
  >   this protocol cannot defend.
  > * The census reports the dynamic-receiver gap; it cannot close it. That
  >   still needs a runtime-visible pin table keyed by shape.
  >
  > **A census build is an INSTRUMENT** — one plain call per site, a string per
  > target, no fast path — and it is never benchmarked, never shipped and never
  > linked into anything that is. `bronze il --census <out>` prints the site
  > table without running anything, which is the cheap way to see what a program
  > would be asked about.
  >
  > **CAMPAIGN CLOSE: what inference cannot see, and the next lever.** Three
  > things, in order of what they cost:
  >
  > 1. **A path the run did not take.** Everything but the four static refusals
  >    is an observation, and a manifest from a thin run is a promise about a
  >    thin run. B1 is what makes that survivable rather than reckless, and it is
  >    why every entry carries `# <n> obs / <m> sites`: forty sites with seven
  >    observations each is a library the run barely touched, and a reader
  >    deciding whether a run was representative wants to know which.
  > 2. **The dynamic-receiver gap**, marked and not closed.
  > 3. **Anything that is not a Number.** Every form in the manifest is about
  >    numbers, so the census's whole vocabulary is numbers. Its refusal comments
  >    are full of shapes it can SEE and cannot SAY — `Material`'s two dozen
  >    monomorphic booleans, `BufferGeometry.attributes`' single object shape,
  >    `Euler._order`'s one string, `Object3D.quaternion`'s one object. Those are
  >    the entries a wider `--pins` grammar would license, and the evidence for
  >    them is already in the file, commented out.
  >
  > **The next lever is unchanged from E4's HANDOFF (a): a scoped escape
  > analysis over environment records.** `env_slot_kernel` is 2.1× off node and
  > the whole of that gap is the record — `bench/env_slot_kernel_registers.js`,
  > same arithmetic and same checksums with the state in bindings no closure
  > captures, puts bronze within 19 % of node. What C1 adds to that case is an
  > input nothing else in the compiler produces: a **per-binding site count**.
  > The manifest says how many distinct stores stand behind each captured
  > binding and how many times each one ran — `function WebGLState.currentSrc:
  > number  # 6000001 obs / 2 sites`. A binding with two sites and six million
  > observations is a record slot that wants to be a register with a write-back;
  > a binding with forty sites and four observations never will be. The census
  > was built to write promises and turns out to also be the profiler the next
  > mechanism needs.

- **Stage B1 (pin enforcement: write barriers)** — 2026-08-25:
  > [!NOTE]
  > **A `--pins` entry was a promise nothing checked: a program that broke one
  > read a pointer's bits as a double and kept going. It now throws a catchable
  > `TypeError` naming the manifest line, at the violating write.** The READ
  > side is untouched — unconditional consumption of the claim is the whole
  > performance model, and a guard there would be the deoptimization the
  > manifest exists to remove — so every line of this stage sits where a claim
  > can be VIOLATED. **The measured tax is ≈ 0**: under 0.05 ns on all three
  > two-count kernels, sign-inconsistent across two independent 101-round
  > sessions, and inside a millisecond harness whose own width on BYTE-IDENTICAL
  > IL is 1.2 ms. Checksums identical in every cell of every table below.
  >
  > **The TDZ elision first, because it is the one place in the compiler where a
  > wrong static answer is SILENT** (E4's HANDOFF (b), item iv). Stage E4's
  > `getDefinitelyAssignedLexicalNames` decides a lexical slot carries no
  > dead-zone marker and its reads carry no check; a wrong "definitely assigned"
  > does not fail loudly, it lets a read of the dead zone through. Absence of a
  > marker is not absence of a claim — the elision IS a claim. The disposition is
  > **PROVEN, not checked**, on two clauses:
  >
  > 1. **Code in a statement list can only read a binding of that list's record
  >    by NAMING it.** The mention scan is `detail::IdentVisitor`, and
  >    `ast::Visitor` declares one PURE virtual per node kind — a form the walk
  >    forgot to descend into stops the build, it does not drop a check. The two
  >    ways JavaScript has of naming a binding without writing its name are both
  >    absent from bronze: `with` is a parse error (`parse/parser_stmt.cpp:197`),
  >    and direct `eval` runs with INDIRECT (global-environment) semantics with a
  >    compile-time warning saying exactly that (`lower/lower_call.cpp:165`).
  > 2. **Any other reader is a closure over this record**, and there are two ways
  >    to get one. A node in the list that BUILDS a function or class — `ReachScan`
  >    stops on sight of `FunctionExpr`, `ClassExpr` or `ClassDecl`, and
  >    `paramDefaultsBuildFunctions` refuses the whole scope if a parameter
  >    default could hold one. Or one of the list's own hoisted `function`
  >    declarations, whose names are in the `stop` set for the WHOLE scan. A
  >    nested `FunctionDecl` is deliberately not flagged as function-building:
  >    `IdentVisitor` walks its body, so every name it could read is collected
  >    here, and a body naming nothing dangerous cannot reach a dangerous binding
  >    however its value is passed around afterwards.
  >
  > The boundary was tested, not asserted.
  > `tests/oracle/cases/dead_zone_reach_boundary.js` is fourteen numbered
  > adversarial cases plus a module-level one, differential against node v24.2.0:
  > a nested declaration reading a later `let`; the same through two hops of
  > hoisted declarations; a closure stored on an object and called later; an
  > object-literal getter; a class expression; try/catch reordering the control
  > flow; a loop whose first iteration reads what a later statement initializes;
  > a mention inside a computed member; a parameter default that builds a
  > closure; a generator suspended across the initializer; a `switch` whose
  > `case` reads a later `let` — and, deliberately, two harmless shapes that must
  > NOT throw, because a scan answering "checked" to everything is sound and
  > worthless. The file is byte-identical to node, and byte-identical again under
  > `BRONZE_NO_DEFINITE_REACH=1` and under `BRONZE_NO_DEFINITE_INIT=1`. Those two
  > seams ARE the checked fallback the charter allowed: a whole-program build
  > with every elided check restored, one flag away, in the shipped binary. Two
  > structural facts fell out of writing the argument down and are worth
  > carrying: `lower/lower_switch.cpp:77` passes an EMPTY statement list, so a
  > switch clause is never definite-init at all, and the module linker flattens
  > every module body into one list whose top-level `function` declarations stay
  > hoisted, so the `stop` set covers them.
  >
  > **Where a barrier goes.** On the STORE and on the ENTRY, and — this is
  > HANDOFF (b) item i — on the logical SLOT, never on the frame region, because
  > E3 merges many logical slots into one GC frame and a frameless callee's write
  > would walk straight past a frame-keyed barrier.
  >
  > | pin | where the barrier is | why there |
  > |---|---|---|
  > | `function <fn>.<slot>: number` | `emitEnvSet`, keyed on the (depth, index) the scope plan resolved | the read is a raw unbox, so the SLOT is what must be true |
  > | `<Class>.<field>: number` / `number-or-nullish` | every store path that reaches a pinned field: plain and compound member assign, the logical/nullish forms, the literal-key index forms, `++`/`--`, destructuring | a boxed store exists, so the store is the cheapest true place |
  > | `<Class>.<field>: numeric-elements` | the field store checks the ARRAY (`bronze_pin_check_array`); every typed element store checks the ELEMENT | the field half is a constructor-time write; the numeric half is one compare per element instead of an O(n) sweep |
  > | `param <owner>(<p>): number`, `return <owner>: number` | the enumerated call sites of the static plan, plus the boxed wrapper; the return at the `return` STATEMENT | HANDOFF (b) item ii: an f64-proof'd parameter has no boxed store at all, so the call site is the only place. The return is checked at the statement because that is where the value is still boxed — the wrapper only ever sees the converted f64 |
  >
  > **And where it does NOT go, which is the load-bearing half.** A barrier is
  > emitted only where lowering has no static answer: `pinSatisfiedStatically`
  > answers yes for any `il::Type::F64` or `I32` value, because an f64 IL value
  > IS a Number by construction. That one test removes the barrier from every
  > arithmetic store, every pinned read fed back into a pinned slot, every typed
  > parameter and every typed call result — the proof is the licence. What it is
  > worth is not an argument, it is a count:
  >
  > | fixture | manifest entries | `pin.guard` emitted | where |
  > |---|---:|---:|---|
  > | `env_slot_kernel` | 12 | **0** | the env-slot fixpoint and E4's parameter proof cover every write in the file |
  > | `call_chain_kernel` | 25 | **1** | `Uniform.setValue`'s `return this.store(...)`, whose result is Dynamic because `flow_expr.cpp` consumes a return pin only for a plain `Ident` callee |
  > | `nullish_pin_kernel` | 4 | **1** | the `Node` constructor's `this.limit = limit` |
  > | `mat4_kernel` | 14 | 332 | **zero** in `Matrix4.multiplyMatrices` and zero in `run` — the entire hot loop, `a.elements[0] = 1.0 + (i & 7) * 0.125` included. All 332 are in cold library methods across 70 functions (`Quaternion.setFromEuler` 24, `Matrix4.set` / `copy` / `multiplyScalar` 16 each), and the single `dense-array` check is in the `Matrix4` constructor |
  > | `tests/oracle/threejs/main.js` | 14 | 473 | the whole vendored library, for scale |
  >
  > **The shape of the check.** A Number's Value bits ARE its double's bits and
  > every other tag sits above the number range, so `number` is one unsigned
  > compare against `BRONZE_ABI_NUMBER_MAX_BITS` and `number-or-nullish` is that
  > plus two constant equalities. Nothing reads the heap, which is what lets the
  > barrier fold away wherever LLVM can already see the value's provenance. The
  > violating edge is a cold block carrying a 1 048 576 : 1 branch weight, so
  > block layout puts it out of line. `numeric-elements`' array test has no
  > inline form — object tag, header read, class compare — so it is one helper
  > call, affordable precisely because that pin's field store is a
  > constructor-time write.
  >
  > **Semantics.** A violation raises through the ordinary convention: the
  > pending exception cell is set and `undefined` is returned, so it is a
  > catchable `TypeError` (`ReferenceError` for TDZ, unchanged), never a process
  > abort. `il::Op::PinGuard` is deliberately OUTSIDE `il::canThrow`'s
  > non-throwing whitelist, so the backend emits the exception check after it
  > like any other raising op. Two consequences the tests pin: the violating
  > store is **dropped**, not performed and then complained about — the assertion
  > is that the slot still reads its old value, because a barrier that let the
  > value through would leave the next raw read of that slot reading a pointer as
  > a double, which is the whole failure this stage closes — and the program
  > keeps running. The message names the manifest LINE as the file spells it
  > (`pin 'Vec.x: number' violated: the value is a string`), with the module
  > linker's `modN.` prefix stripped, because the message exists to be grepped
  > for in the file that caused it. One further semantic change: a pinned
  > PARAMETER's boxed wrapper used to run ToNumber, so `scale("4")` quietly saw
  > 4; it now tag-tests and throws. `src/types/pins.h` called the signature forms
  > the weakest pin in the file for exactly that reason, and no longer does.
  >
  > **The seam.** `BRONZE_NO_PIN_BARRIERS=1` removes every barrier and restores
  > the older undefined behaviour, so the tax peels out of ONE binary. With it
  > set, the IL of all four pinned kernels differs from today's only by the
  > absent `pin.guard` instructions and the key-constant indices they intern
  > (checked by diff; a `param`/`return` entry interns its manifest line even
  > where the proof then deletes the barrier, which leaves a few dead key
  > constants and no code). It is the INNERMOST seam of the campaign ladder and
  > is on in every earlier column, because none of the compilers those columns
  > reproduce emitted a barrier: `e5` with it set is E5's shipped compiler, and
  > the new `b1` column is the one with no seams at all.
  >
  > **THE LADDER, SEVEN COLUMNS, one idle session, one compiler binary**
  > (`bash bench/tools/ladder.sh <bronze.exe> <dir>`, then `ladder_specs.py` and
  > `interleave.py`; `selftimed.py` for `call_chain`, `nodebench.py` for the node
  > column, all in the same session). Kernels at 101 rounds by two-count wall
  > delta, millisecond fixtures at 51 rounds raw.
  >
  > | | stage 3.4 | E1 | E2 | E3 | E4 | E5 | **B1** | node v24.2.0 |
  > |---|---:|---:|---:|---:|---:|---:|---:|---:|
  > | `env_slot_kernel` ns/iter | 56.12 | 47.60 | 14.82 | **10.51** | 10.64 | 10.58 | **10.60** | 5.08 |
  > | `mat4_kernel` ns/call | 23.42 | 23.35 | 17.07 | 16.09 | 17.91 | **16.02** | **16.07** | 14.59 |
  > | `nullish_pin_kernel` ns/step | 12.80 | 12.78 | 11.29 | **11.13** | 11.13 | 11.17 | **11.16** | 5.51 |
  > | `call_chain_kernel` chained / flat | 19.88 / 18.38 | 19.88 / 18.50 | 11.25 / 12.50 | 9.25 / 9.75 | 9.38 / 8.50 | **9.25 / 8.50** | **9.50 / 8.75** | 2.00 / 1.88 |
  > | `typed_array_crunch` ms | 52.28 | 52.11 | 46.59 | 46.60 | **35.17** | 35.23 | **35.42** | 55.96 |
  > | `three_math` ms | 41.32 | **41.13** | 42.90 | 42.01 | 41.76 | 42.79 | **41.77** | 50.82 |
  > | `mesh_churn_2k` ms | 73.94 | 73.83 | 74.16 | 72.08 | **70.66** | 71.10 | **71.04** | 93.12 |
  > | `object_graph` ms | 47.10 | 46.66 | 46.45 | 46.88 | **46.25** | 46.44 | **46.40** | 71.30 |
  >
  > Checksums `126000020 / 12600020`, `400000 / 940000`,
  > `825756/700159/NaN/-563350`, `296000000 / 296000000`, `78849652`, `405000`,
  > `-2112298`, `-32601148` — every cell of every column, node's included. Read
  > DOWN a column: this session runs ~1 % faster than E5's (its E3 `mat4` column
  > reads 16.09 here against 17.00 there), which is why nothing across sessions
  > is claimed.
  >
  > **THE TAX, measured on its own.** The ladder's B1 column already carries it,
  > but a seven-column table is a bad instrument for a difference this small, so
  > the A/B was also run alone — two columns, same binary, `E5` = barriers off,
  > `B1` = barriers on, interleaved, and repeated in a second independent
  > session:
  >
  > | fixture | guards | barriers off | barriers on | delta | repeat delta |
  > |---|---:|---:|---:|---:|---:|
  > | `env_slot_kernel` ns/iter | 0 | 10.808 | 10.763 | **−0.045** | +0.028 |
  > | `mat4_kernel` ns/call | 332 | 16.069 | 16.086 | **+0.017** | +0.041 |
  > | `nullish_pin_kernel` ns/step | 1 | 11.196 | 11.154 | **−0.042** | −0.020 |
  > | `call_chain_kernel` chained ns | 1 | 9.25 | 9.50 | **+0.25** | +0.12, +0.13 |
  > | `call_chain_kernel` flat ns | **0** | 8.50 | 8.75 | **+0.25** | +0.12, +0.13 |
  > | `typed_array_crunch` ms | 0 (no manifest) | 36.665 | 36.590 | −0.075 | — |
  > | `three_math` ms | 0 (no manifest) | 44.323 | 43.169 | −1.154 | — |
  > | `mesh_churn_2k` ms | 0 (no manifest) | 72.618 | 73.099 | +0.481 | — |
  > | `object_graph` ms | 0 (no manifest) | 47.315 | 47.290 | −0.025 | — |
  >
  > **The bottom four rows are a NULL CONTROL and they are the most useful rows
  > in the table.** Those four fixtures are built with no manifest, so no barrier
  > can exist in either column and their IL is byte-identical across the seam
  > (verified by diff on all four). The spread they show — **−1.15 to +0.48 ms**
  > — is therefore the millisecond harness's own width in this session, and it is
  > larger than anything the barriers do. `three_math` moving −1.15 ms on
  > byte-identical IL is the single clearest statement of that.
  >
  > `call_chain_kernel` is the one fixture with a barrier on a hot path — the
  > return guard runs on all 8 M iterations, because `i & 15` misses the cache
  > every time — and it reads +0.12 to +0.25 ns three sessions running. But the
  > **`flat` arm, which contains no barrier at all**, moves by exactly the same
  > amount in the same direction in all three. So the shift is the binary, not
  > the check: at the fixture's 0.125 ns timer granularity the guard itself is
  > not resolvable. It is reported as a real (if tiny) cost of shipping the
  > barriers on this fixture, and NOT as the cost of the guard.
  >
  > **COMPILE TIME.** Medians of five interleaved `--timings` wall builds (four
  > for the largest):
  >
  > | build | LLVM insts | partitions | guards | barriers off | barriers on | delta |
  > |---|---:|---:|---:|---:|---:|---:|
  > | `three_math` (no manifest — IDENTICAL IL) | 582 002 | 2 | 0 | 22.64 s | 22.85 s | +0.9 % |
  > | `mat4_kernel --pins` | 455 018 | 2 | 332 | 13.02 s | 13.21 s | +1.5 % |
  > | `tests/oracle/threejs/main.js --pins` | 1 698 990 | 8 | 473 | 31.89 s | 32.13 s | +0.8 % |
  >
  > The first row is again the null control, and at +0.9 % on identical work it
  > says the compile-time harness cannot resolve what the other two rows are
  > measuring. Barrier emission is not a measurable compile-time cost.
  >
  > **Partition placement was checked FIRST**, per E5's handoff, and it DID move:
  > the barriers add 3 335 LLVM instructions to `mat4_kernel` (451 683 → 455 018,
  > +0.74 %) and the cross-partition keep sets change with them (p0/p1 borrow
  > 13 / 7 bodies without barriers and 9 / 9 with). Both builds stay at two
  > partitions and both keep the multiply inlined — the `mat4` column reads 16.07
  > against E5's 16.02, nowhere near E4's un-inlined 17.91 — so the shift is
  > visible and harmless. It is exactly the failure mode E5 warned door 2 about,
  > and it is why that check is in this entry rather than in a footnote.
  >
  > **Honest negatives.**
  >
  > 1. **Enforcement is not TOTAL, and the hole has a name.** A field pin is
  >    resolved for the barrier exactly the way the READ resolves it — receiver's
  >    shape class, fallback to the interned constructor name, walk up `extends`
  >    — deliberately, because two different answers would be a promise spent
  >    with no barrier. The price is that a store through a receiver inference
  >    types `dynamic` gets NO barrier, while a class-known read elsewhere still
  >    spends the claim. In one program with `Vec.x: number` pinned,
  >    `function putKnown(v, y) { v.x = y; }` emits
  >    `pin.guard %1, number, "Vec.x: number"` and
  >    `function putBlind(o, y) { o.x = y; }` emits nothing. Closing it needs a
  >    RUNTIME-visible pin table keyed by shape, so an unknown-receiver store can
  >    ask; that is a door-3 design item. `src/types/pins.h` states it where a
  >    manifest author will read it.
  > 2. **Two claims are unchecked by construction rather than by omission.** The
  >    ELEMENTS of an array assigned wholesale into a `numeric-elements` field
  >    are not swept — that is an O(n) walk at a store whose entire purpose is an
  >    O(1)-per-element loop after it — so the numeric half is held one element
  >    at a time, which covers every element bronze itself writes and not one an
  >    `Array.prototype.fill`, a host, or a `JSON.parse` result put there. And
  >    the IN-BOUNDS half of the element claim has no write to be held at, since
  >    in-bounds is a claim about a read; a bounds check on the store is the
  >    guard the pin exists to delete.
  > 3. **`mat4_kernel` ships 332 barriers.** The ≈0 target is met on the hot
  >    path and is NOT a statement about a fixture-wide count. It happens to cost
  >    nothing measurable here because those 332 are in library methods called a
  >    handful of times each, but a program whose cold code is its hot code would
  >    pay, and this entry does not claim otherwise.
  > 4. **The one guard on a hot path is pure tax today, and need not stay so.**
  >    `Uniform.setValue`'s `return this.store(...)` is guarded and then unboxed
  >    with a CHECKED `unbox.f64` — the return pin's claim is not spent at that
  >    site, because `flow_expr.cpp` consumes a return pin only for a plain
  >    `Ident` callee. Teaching it the method-call form would turn this barrier
  >    into a net saving rather than a net cost. Out of scope here (it is a read-
  >    side change), recorded because it is the one place the stage adds a check
  >    without deleting one.
  > 5. **A pre-existing MISCOMPILE, found while writing the tests and unrelated
  >    to pins**, fixed in its own commit. `functionRefMap_` memoizes the
  >    `func.ref` a mention of a top-level function lowers to, holding an
  >    INSTRUCTION RESULT ID; `lowerFunctionBody` cleared it on the way into a
  >    nested body and `lowerClosure` never restored it, so the enclosing
  >    function came back holding the callee's ids.
  >    `(function () { h(g, 4); })(); h(g, 3);` called the anonymous function
  >    twice and never called `h` again — silently, because the id is always in
  >    range and the verifier sees nothing wrong. It was found because the pin
  >    tests' own `report(label, fn)` harness is that exact shape. No fixture and
  >    no file of the three.js oracle changes a byte of IL under the fix, which
  >    is why no published number moves; the shape is ordinary JavaScript all the
  >    same.
  >
  > **Tests.** `tests/cli/pin_barrier_test.cpp`, nine cases against the driver,
  > asserting the SHAPE off `runIl` (which stores carry a `pin.guard` and — as
  > hard — which do not) and the BEHAVIOUR off `runBuild` and the produced
  > executable (the throw is a TypeError, it is catchable, it names the manifest
  > line, the program keeps running, the violating value was not stored): a
  > violating env-slot write, a fixpoint-proved slot that carries no barrier, a
  > violating field write, a `number-or-nullish` field that admits `null` and
  > `undefined` and refuses a string, a `numeric-elements` field and element, a
  > field store already typed f64 that carries no barrier, a violating argument
  > through the boxed wrapper, a violating return, and the message reading back
  > as a line of the manifest. The seven barrier-expecting cases skip under
  > `BRONZE_NO_PIN_BARRIERS=1` — the seam is a measurement instrument, not a
  > contract configuration, and it is read once per process so one run cannot
  > have both — while the ABSENCE cases stay on, because "no barrier here" had
  > better not depend on the seam. **ctest 29/29 in BOTH contract configurations
  > (default and `BRONZE_ELIDE_ENV_GUARDS=1`)**, and the codegen, IL, lower,
  > types, cli and both oracle suites green again under
  > `BRONZE_NO_PIN_BARRIERS=1`.
  >
  > **HANDOFF (d): what door 3 (census-driven manifest inference) should know now
  > that pins are enforced.**
  >
  > 1. **An inferred pin CAN ship enforced-by-default, and that is the main thing
  >    this stage changes for the census.** Before B1 a wrong inferred entry
  >    compiled to a raw unbox of a pointer, so a census would have had to be
  >    right in the "no false positives, ever" sense before anything it emitted
  >    could ship. It now only has to be right about the HOT path: a wrong entry
  >    on a cold path is a TypeError at the violating write, catchable, naming
  >    the line it came from. That turns a proof obligation into a
  >    precision/recall problem with a bounded downside, and it is what makes a
  >    census worth building. The caveat is negative 1 — "enforced by default"
  >    means "enforced wherever the compiler knows the receiver's shape".
  > 2. **Emit the manifest LINE, not just the fact.** Every barrier names its
  >    entry as the manifest spells it, prefix stripped, so a violation in the
  >    field is a one-line diff back to the manifest it came from. A census
  >    should emit entries in exactly that spelling — `Vector3.x: number`,
  >    `param Uniform.setValue(x): number` — or the loop does not close.
  > 3. **A SECOND enumeration for the census, of the same kind as the first.**
  >    HANDOFF (c) said the census's real job is enumerating call sites of
  >    ESCAPED closures, and that stands. B1 adds: the stores that reach a pinned
  >    field through a receiver the compiler cannot type. A dynamic census sees
  >    the shape at those sites for free — same instrumentation — and it is the
  >    only way to close the last silent hole short of a runtime pin table. So
  >    the census's output should distinguish "this field is always a number"
  >    from "this field is always a number AND every store to it is from a site
  >    the compiler can type", because only the second is fully enforced today.
  > 4. **The proof still runs first**, unchanged from HANDOFF (c). What B1 adds
  >    is a cheap way to MEASURE the overlap it warned about: a proposed entry
  >    that produces zero `pin.guard` instructions is one the proof already
  >    covers, and `bronze il --pins <file>` reports that without running
  >    anything.
  > 5. **The seam is the census's A/B too.** `BRONZE_NO_PIN_BARRIERS=1` gives a
  >    binary with the same IL minus the barriers, so a census can price its own
  >    proposals — build with the inferred manifest, count guards, peel — without
  >    a second compiler.

- **Stage E5 (the split stops eating the inline plan)** — 2026-08-25:
  > [!NOTE]
  > **One mechanism, and it is not an optimization — it is a leak in the
  > build.** `mat4_kernel` goes **18.31 → 16.43 ns/call** with every E4
  > mechanism left ON, which is not only the whole of E4's regression but
  > **0.57 ns past** the E3 column measured in the same session (17.00). The
  > kernel's IL did not change. What changed is that a callee in another
  > emission partition is no longer a bare `declare`. Checksums identical in
  > every column of every row.
  >
  > **What E4 found and this stage confirmed unchanged.** bronze splits a
  > module over 400 000 instructions into per-thread partitions by greedy
  > largest-first bin packing (`src/codegen-llvm/llvm_backend.cpp`), and the
  > worker loop called `f.deleteBody()` on every function outside its bin. The
  > assignment had never heard of a call edge. Re-verified here from the IR
  > before touching anything: in `mat4_kernel` (452k instructions, 2
  > partitions) `run` is defined in `p0` while
  > `mod1.Matrix4.multiplyMatrices.inl` is defined in `p1`, so `p0` held
  > `declare hidden i64 @"__bronze_part$mod1.Matrix4.multiplyMatrices.inl"` and
  > the `alwaysinline` stage 3.3 put on that call site was unsatisfiable. E4's
  > root cause held up **exactly**; nothing in it needed amendment.
  >
  > **The repair** (`src/codegen-llvm/llvm_partition.{h,cpp}`, seam
  > `BRONZE_NO_XPART_INLINE=1`). A body a bin does not own but does CALL is kept
  > in that bin as `available_externally` instead of deleted. That linkage is
  > exactly the shape of the claim: a definition the optimizer may read and
  > copy, and the emitter must not emit, because the bin that owns it will.
  > The pipeline's own `EliminateAvailableExternallyPass` — part of
  > `buildPerModuleDefaultPipeline`, after the inliner and before the MC
  > backend — drops the kept body back to a declaration, so every partition
  > object still defines only what it owns. Nothing is compiled twice, and the
  > link is the oracle for that: a body emitted from two partitions is a
  > duplicate-symbol error, not a silent doubling.
  >
  > The IR, before and after, in `p0` — the caller's partition:
  >
  > ```
  > ; before, p0 pre-O3
  > declare hidden i64 @"__bronze_part$mod1.Matrix4.multiplyMatrices.inl"(i64, i64, i64, ptr "bronze.frame_region", ptr "bronze.tls_block")
  > ; after, p0 pre-O3
  > define available_externally hidden i64 @"__bronze_part$mod1.Matrix4.multiplyMatrices.inl"(i64 %__this, i64 %a, i64 %b, ptr "bronze.frame_region" %__region, ptr "bronze.tls_block" %__tls) {
  > ; after, p0 post-O3 — no definition, no declaration, no call: inlined and gone
  > ```
  >
  > `run` post-O3 goes from 2 341 to 3 826 lines of IR, which is the multiply
  > arriving. The E2–E4 attribute machinery travels with the body untouched:
  > the `bronze.frame_region` and `bronze.tls_block` parameter attributes are
  > there verbatim above, because a kept body is not rewritten — it is the same
  > bitcode every partition parses, and the ONLY per-partition mutations in the
  > backend are this linkage choice, `deleteBody`, the DLL storage class, and
  > partition 0's ownership of global initializers. That last one is a
  > difference in what the optimizer KNOWS (a global's initializer is visible
  > in `p0` and not in `p7`), never in what a body SAYS, and it predates this
  > stage: every partition already referenced those globals from its own
  > members.
  >
  > **THE CAP, and it is not a taste question.** The shipped cap is **2048**
  > instructions and it is forced, not chosen: `Matrix4.multiplyMatrices.inl`
  > is 1 782 instructions, and the site-level direct-edge budget
  > (`markDirectMethodInlining`, stage 3.3) is also 2048. A cap of 1024 leaves
  > the body outside and the regression completely unrepaired. 101 rounds,
  > interleaved, one session:
  >
  > | `mat4_kernel` | ns/call | compile (452k insts, 2 partitions) |
  > |---|---:|---:|
  > | xpart off (= E4) | 18.318 | 15.15 s |
  > | cap 1024 | **18.311** | 15.46 s |
  > | cap 2048, depth 1 | 16.457 | 17.11 s |
  > | cap 2048, depth 2 (**shipped**) | **16.448** | 17.38 s |
  > | cap 2048, every direct call | 16.428 | 19.06 s |
  >
  > Checksums `400000 / 940000` in all five. The right way to read the cap is
  > that it must EQUAL the site budget: the set this mechanism exists to
  > protect is exactly the set that budget admits, so any smaller cap silently
  > un-protects part of it and any larger one carries bodies no site would
  > inline anyway.
  >
  > **WHICH callees, and the wide rule loses.** E4 sketched "direct-call
  > callees under a size cap". Measured, the narrow rule is strictly better:
  > keep only the callees of sites the compiler ALREADY marked `alwaysinline`
  > — the direct method and closure edges stages 3.3 and E1 built, which is
  > precisely the set a blind split can silently un-inline. On `mat4_kernel`
  > that is 20 bodies against 133 for every direct call, it reads the same
  > 16.43 against 16.43, and it costs **1.7 s less compile** (17.38 s against
  > 19.06 s).
  > `BRONZE_XPART_INLINE_MODE=all` is the seam that took that column. So E4's
  > sketch needed narrowing — not correcting.
  >
  > **DEPTH, and the one thing here no fixture decides.** Every member of a bin
  > is in the depth-0 frontier, so depth 1 already means "every direct callee
  > of anything this bin owns"; depth only buys the hops through bodies that
  > are themselves kept — `a → b → c` split across three bins, which is E3's
  > nested-inline-edge shape. **No fixture distinguishes depth 1 from depth 2**
  > (16.457 vs 16.448 on `mat4_kernel`, 44.87 vs 45.05 on `three_math`, 77.88
  > vs 77.55 on `mesh_churn_2k` — all inside the width). Depth 2 ships anyway,
  > on two grounds that are arguments and not measurements, and are labelled as
  > such: the shape is real and populated (going 1 → 2 keeps 28 more bodies on
  > `mesh_churn_2k`, i.e. 28 second-hop chain edges that exist), and the point
  > of the mechanism is to make the split invisible to the inline plan, which a
  > depth-1 rule leaves one shape short of. It costs one extra body on the
  > two-partition fixtures. `BRONZE_XPART_INLINE_DEPTH=<n>` is the seam.
  >
  > **The ladder, six columns, one session, one compiler binary** (the E4
  > column is today's compiler with `BRONZE_NO_XPART_INLINE=1`; `bench/tools/`
  > carries the harness, and `ladder.sh` now peels six stages instead of five).
  > Kernels at 101 rounds, millisecond fixtures at 51.
  >
  > | | stage 3.4 | E1 | E2 | E3 | E4 | E5 |
  > |---|---:|---:|---:|---:|---:|---:|
  > | `mat4_kernel` ns/call | 24.50 | 23.98 | 17.34 | 17.00 | 18.31 | **16.43** |
  > | `env_slot_kernel` ns/iter | 56.62 | 47.38 | 15.12 | 10.86 | 10.92 | 10.90 |
  > | `nullish_pin_kernel` ns/step | 13.09 | 13.10 | 11.54 | 11.40 | 11.41 | 11.42 |
  > | `call_chain_kernel` chained / flat | 20.50 / 19.00 | 20.50 / 19.00 | 11.63 / 12.88 | 9.63 / 10.00 | 9.63 / 8.88 | 9.63 / 8.75 |
  > | `typed_array_crunch` ms | 54.70 | 54.63 | 49.35 | 49.45 | 37.74 | 37.91 |
  > | `three_math` ms | 43.86 | 45.92 | 43.04 | 45.80 | 45.67 | **44.64** |
  > | `mesh_churn_2k` ms | 77.44 | 79.05 | 78.73 | 75.56 | 74.13 | 75.31 |
  > | `object_graph` ms | 48.81 | 48.90 | 48.46 | 49.09 | 48.44 | 48.95 |
  >
  > Checksums `400000 / 940000`, `126000020 / 12600020`,
  > `825756/700159/NaN/-563350`, `296000000`, `78849652`, `405000`,
  > `-2112298`, `-32601148` — every cell of every column. This session runs
  > about 1 % slower than E4's across the board (E3's `mat4` column reads 17.00
  > here against 16.25 published, E4's 18.31 against 18.09), which is why the
  > claim to read off this table is the WITHIN-session one: E5 is 1.88 ns under
  > E4 and 0.57 under E3.
  >
  > **Only three fixtures in the suite can be touched by this at all**, and
  > saying so is most of the honesty in the table. `typed_array_crunch`
  > (10 587 instructions), `object_graph` (34 683), `env_slot_kernel` (2 236),
  > `nullish_pin_kernel` (9 579) and `call_chain_kernel` (11 071) are all below
  > the 400 000-instruction partition threshold, compile to one object, and
  > never reach this code — their `BRONZE_NO_XPART_INLINE=1` and default LLVM
  > IR are byte-identical (`cmp` on the pre- and post-O3 dumps, no diff). The
  > three that partition are `mat4_kernel` (2), `three_math` (2) and
  > `mesh_churn_2k` (8).
  >
  > **THE HONEST NEGATIVE: `mesh_churn_2k` pays about 1 ms.** Three
  > independent interleaved sessions put it at +1.18, +0.86 and +1.02 ms
  > (+1.2 – 1.6 %), and it is not the depth or the mode — off 76.53, depth 1
  > 77.88, depth 2 77.55, every-direct-call 77.53. It is inlining's ordinary
  > tax: the fixture touches 1 811 functions and its binary grows **9.44 →
  > 11.21 MB** (+19 %). `three_math`, the other partitioned fixture, moves the
  > other way by about the same amount (−1.03, −1.33 and −1.45 ms across the
  > same three sessions, binary 4.72 → 5.06 MB). So the mechanism is not free on a
  > fixture with no hot cross-partition edge, and the campaign should carry
  > that: it repairs a broken inline plan, it does not improve one that was
  > intact.
  >
  > **COMPILE TIME, measured because a kept body costs one.** Interleaved,
  > median of three, `--timings` wall:
  >
  > | build | instructions | partitions | off | E5 | borrowed |
  > |---|---:|---:|---:|---:|---|
  > | `three_math` | 582 002 | 2 | 20.29 s | **23.45 s** (+16 %) | 23 fns / 24 519 insts |
  > | `tests/oracle/threejs/main.js` | 1 808 946 | 9 | 20.77 s | **25.38 s** (+22 %) | 250 fns / 243 050 insts |
  >
  > Medians of five, interleaved. Below 400 000 instructions the cost is
  > exactly zero, because the split never happens. Above it, the tax is
  > 16 – 22 % of a build that was already parallel, and it buys back an inline
  > the same build had silently lost.
  >
  > **Determinism.** Partition contents feed nothing that is hashed — the ABI
  > stamp is `BRONZE_ABI_FINGERPRINT`, a constant of the header the compiler
  > was built against, not a digest of any emitted object — so the requirement
  > here is reproducibility, not identity with a recorded value. The keep sets
  > are a fixpoint over a bin assignment whose only tie-break is the symbol
  > NAME, and `planPartitions` called twice on one module returns equal plans
  > (unit test, including the two equal-sized functions the module lists in the
  > wrong order). Through the real compiler: two builds of `mat4_kernel` write
  > byte-identical `p0` and `p1` pre-O3 IR, 13 `available_externally` bodies in
  > `p0` and 7 in `p1` both times. Byte-comparing two whole EXECUTABLES proves
  > nothing on Windows either way: the linker stamps a timestamp, so two builds
  > of one source at one configuration already differ at byte 129.
  >
  > **The five checks** (`tests/codegen_llvm/codegen_llvm_test.cpp`), on
  > hand-built modules, because what has to hold is a statement about a PLAN
  > and about LINKAGE: a cross-bin direct-call callee is kept; an over-cap one
  > and an uncalled one are not; a call with no inline request does not drag a
  > body across; a chain is followed to the shipped depth and no further; and
  > the end-to-end one — after `applyPartition` the callee is
  > `available_externally` with a body and no dllexport, the module verifies,
  > and after the real O3 module pipeline the caller holds no call to it and
  > the module holds no definition of it. Every one of them has a
  > `BRONZE_NO_XPART_INLINE=1` arm, so the seam column is green too. Both
  > contract configurations — default and `BRONZE_ELIDE_ENV_GUARDS=1` — are
  > green.
  >
  > **HANDOFF: what door 2 (pin enforcement) learns from this.** Nothing about
  > store paths, and one thing about ITS OWN measurements. Every claim door 2
  > will make about a barrier's cost is a difference between two compiled
  > binaries, and until today two binaries built from IL that differed
  > ANYWHERE could differ in which functions got inlined, for reasons having
  > nothing to do with the change under test — a barrier that grows a function
  > by 30 instructions could tip the packing and un-inline a hot callee three
  > functions away, and the measurement would read that as the barrier's cost.
  > That failure mode is now closed for the direct edges; it is NOT closed for
  > LLVM's own cost-based inlining across a split, which `BRONZE_XPART_INLINE_MODE=all`
  > would cover and which measured worth nothing today. If a door-2 A/B on a
  > partitioned fixture ever shows a cost that the IL cannot explain, check the
  > partition placement FIRST — `--timings` now prints the borrowed count per
  > partition, and `BRONZE_DUMP_LLVM_IR` names the bin of every definition.

- **Stage E4 (a parameter proof for closures, predicate attributes, and a dead zone nothing can reach through)** — 2026-08-25:
  > [!NOTE]
  > **The stage's most valuable output is not a speedup, it is three
  > measurements that close questions the campaign has been carrying.** What
  > shipped: `typed_array_crunch` **47.97 → 36.95 ms** (−23 %), `mesh_churn_2k`
  > 73.98 → **72.21**, `env_slot_kernel` 10.94 → **10.91** (nothing), and
  > `mat4_kernel` 16.25 → **18.09**, a regression this entry root-causes and
  > does not smooth. Checksums identical in every column of every row.
  >
  > **1. A PARAMETER PROOF for closures** (`src/lower/lower_scope.cpp`,
  > `planClosureParamNumbers` / `applyProvenClosureParams`, seam
  > `BRONZE_NO_CLOSURE_PARAM_PROOF=1`). Stage E3 wrote down the hole: a class
  > method's parameters are typed by joining what every call site passes, a
  > module function's by the same join over its enumerated callers, and a nested
  > `function f() {}` by NOTHING — it is reached through a function value, so
  > inference has no signature that can speak for its arguments.
  > `bench/pins/env-slot-kernel.pins` carried five `param` lines that existed
  > only because of that hole.
  >
  > But a nested declaration's callers are enumerable on exactly the terms E1's
  > static call plan already establishes for its VALUE: the declaration IS the
  > value, it is installed before the scope's first statement runs, and the
  > scope's whole lexical reach is one subtree this compilation holds. So walk
  > that subtree once; if EVERY mention of the name is the callee of an ordinary
  > call, those calls are all the calls, and the join over their arguments at
  > position k is a fact about every value parameter k will ever hold. That is a
  > proof, not a pin, and every clause is a refusal in the safe direction: a
  > plain declaration only (a generator's parameters are bound by a resume edge,
  > not by the call); no default, rest or pattern parameter (the value bound is
  > then not the value passed — the three positions `applySignaturePins` refuses,
  > and for the same reason: there is no `undefined` in an f64); the name rebound
  > nowhere; no spread at any site; and every site supplies position k with a
  > proven Number, so a single SHORT call refuses the position because it binds
  > `undefined`.
  >
  > **The result is worth 0.00 ns on `env_slot_kernel`, and that is the finding.**
  > The module it builds without the five `param` pins is **byte-identical LLVM
  > IR** to the module built with them (`cmp`, no diff) — the proof reaches
  > exactly the claim the manifest was making by hand, on a fixture written to
  > need it. Without either, the wrapper carries 571 live instructions instead of
  > 313. What it does NOT reach is the closure a factory HANDS OUT:
  > `env_slot_kernel` ends `return render;`, so `render`'s own `iters` escapes
  > through a door the enumeration cannot follow and stays pinned. That is the
  > honest boundary — it types the closures a factory CALLS, not the one it
  > returns — and it is the single most useful thing door 3 can be told.
  >
  > **2. `memory(read)` on the predicate helpers** (`src/codegen-llvm/llvm_abi.cpp`,
  > seam `BRONZE_NO_PURE_PREDICATES=1`). `bronze_truthy` and `bronze_unbox_bool`
  > become `onlyReadsMemory` + `willReturn`; `bronze_is_nullish`, which touches
  > nothing at all, becomes `doesNotAccessMemory` + `speculatable`. Not
  > `memory(none)` for the first two: truthy READS the heap — a string's length,
  > a BigInt's limbs. `bronze_strict_eq` is excluded because it calls
  > `recordHelperCall`, which is a counter WRITE; `bronze_typeof` can intern;
  > `bronze_rel_*` can run `valueOf`. GC soundness rests on the heap being
  > non-moving and on `memory(read)` not being sinkable past an opaque call.
  >
  > Stage E2 proved an attribute-only change can be worth more than the code it
  > describes (23.4 ns from `memory(none)` on ToInt32), and this one is the
  > counter-example: it takes the `env_slot` loop from **261 to 143 static
  > instructions** and lets LLVM unswitch the record's brand-and-size guard chain
  > out of the loop entirely — and moves the clock **0.03 ns**, which is nothing.
  > A 45 % cut in static loop size buying zero is itself the evidence for the
  > handoff verdict below.
  >
  > **3. The dead zone widened from a PREFIX to a REACH** (`src/ast/queries_declaration.cpp`,
  > seam `BRONZE_NO_DEFINITE_REACH=1`). Stage E3's definite-assignment rule was
  > the prefix of a scope's statement list that runs no user code, which is
  > exactly why `typed_array_crunch` still gave up 4.5 ms to
  > `BRONZE_ELIDE_ENV_GUARDS=1` after E3 shipped: `runNBody`'s FIRST statement
  > is `const rng = makeLCG(12345)`, a call, so the prefix was empty and all 41
  > of that scope's env reads stayed `bronze_env_get_tdz`. The rule now advances
  > past any statement that neither BUILDS A FUNCTION nor MENTIONS a name still
  > in the dangerous set — a call cannot observe a dead zone it has no way to
  > name, and only a function built before the initializer can carry the name
  > somewhere later. It takes that scope to **0** tdz reads, and it is worth
  > **11.0 ms** on `typed_array_crunch`, which SUBSUMES the whole elision gap:
  > `BRONZE_ELIDE_ENV_GUARDS=1` on top of it now adds 0.27 ms where it added
  > 4.83 before. `object_graph` goes 9 → 3 tdz reads, `three_math` 120 → 92,
  > `mesh_churn_2k` 970 → 839.
  >
  > **THE REGRESSION, and it is not the analysis.** `mat4_kernel` reads 18.09
  > against E3's 16.25. A single-seam A/B over 101 rounds puts it entirely on
  > this mechanism (`E4-noReach` 16.33, `E4-noPurePred` 18.14, `E4-noParamProof`
  > 18.24, `E4` 18.21) — and then the IR says the mechanism did nothing to the
  > hot function at all. `run`'s own body is unchanged either way: 3
  > `bronze_env_get_tdz`, same calls, same shape. What changed is which
  > PARTITION it landed in. bronze splits a large module into per-thread
  > partitions by greedy largest-first bin packing on instruction count
  > (`src/codegen-llvm/llvm_backend.cpp`), and the assignment is completely
  > inlining-blind: with the widening on, `run` is defined in `p0` while
  > `Matrix4.multiplyMatrices.inl` is defined in `p1`, so the hot callee is a
  > `declare` in `run`'s partition and cannot be inlined (2064 instructions in
  > `run`); with it off, both land in `p1` and it inlines (3398). The widening
  > shrank OTHER functions enough to tip the bin packing.
  >
  > **That is the campaign's most important loose thread and it belongs to no
  > stage in it.** Every stage from 3.3 on has argued about inlining across a
  > direct edge, and a bin-packer that has never heard of the edge can silently
  > take the inline away from any of them, in either direction, whenever IR size
  > moves. It plausibly explains some of the cross-session drift E2 and E3
  > blamed on machine state. The fix is small and local: the worker loop calls
  > `f.deleteBody()` on every function outside its bin, and a body kept as
  > `available_externally` instead — for direct-call callees under a size cap —
  > is inlinable without being emitted twice. That is a mechanism with its own
  > seam and its own measurement, and it is the first thing the next campaign
  > should build. *(Built as stage E5, above. The root cause held exactly; the
  > sketch needed narrowing — "direct-call callees" turned out to be wider than
  > necessary, and the callees of sites already marked `alwaysinline` buy the
  > same nanoseconds for a fifth less compile time.)*
  >
  > **The residual that measured zero and was dropped.** E3 reported two
  > `bronze_dynamic_add` in the `env_slot` kernel and got it to one, leaving the
  > last as an open item. There is nothing to remove: the survivor is not in the
  > loop. It is `total() + hits` in the epilogue, reached from the loop's EXIT
  > block, executed once per program. E3's census counted over the whole
  > wrapper and read a once-per-run instruction as a per-iteration one.
  >
  > **What the ladder reproduced, and the two cells it did not.** Peeling the
  > seams reproduces the published stage numbers closely enough to trust the
  > table: stage 3.4's `env_slot` 56.38 against 56.70 published, its
  > `call_chain` 20.25 / 18.75 against 20.00 / 18.25, E1's `call_chain`
  > unmoved exactly as E1 reported, E2's `env_slot` 15.51 against 16.78, E3's
  > 10.94 against 10.77 and its `mat4` 16.25 against 16.13. Two cells do not.
  > E1's `env_slot` column reads **48.36** where stage E1 published **50.82** —
  > the seam-off compiler is TODAY's compiler with E1's mechanism disabled, so
  > it carries every later stage's work that E1's binary did not, and the two
  > are not the same object; a peeled ladder is a clean set of DIFFERENCES and
  > only approximately a set of historical absolutes. And `mat4` at E4 is the
  > regression above. Where a peeled cell and a published cell disagree, the
  > peeled one is the honest number for reading DELTAS down a column and the
  > published one is the honest number for what that stage shipped.
  >
  > **A test-matrix correction the campaign should stop repeating.** The
  > standing instruction to re-run the suite "with all seams on" cannot be
  > green and never has been: the seams turn the mechanisms off and the
  > mechanisms' own unit tests assert their IL. With only the E1–E3 seams and
  > nothing of E4's, `bronze_lower_tests` already fails 6 cases / 12 assertions
  > (E1's `lower_stable_closure_call_test`, E3's `lower_scope_test`); E4's two
  > seams add its own two. The two configurations that ARE contracts, and are
  > both **29/29** here, are the default one and `BRONZE_ELIDE_ENV_GUARDS=1`.
  > The all-seams build is a measurement instrument, not a supported build.
  >
  > **The harness is now in the tree** (`bench/tools/`): `ladder.sh` builds the
  > columns, `ladder_specs.py` writes the specs so column order and iteration
  > counts cannot drift between rows, `interleave.py` is the protocol E3
  > established (interleaved, medians, warmup discarded, 101 rounds),
  > `selftimed.py` reads `call_chain_kernel`'s own printed nanoseconds, and
  > `nodebench.py` takes the oracle column under the same protocol. Stage E3's
  > round-count finding is enforced by documentation in `interleave.py` rather
  > than being re-derived; the one thing worth repeating here is that
  > `ladder.sh` iterates its columns BY TAG, because a `for col in $COLS` over
  > `tag:seams` pairs splits on the spaces inside a seam set and silently builds
  > a table that is not the ladder — 85 executables instead of 55 is the only
  > tell, and this stage built that table once before noticing.
  >
  > **HANDOFF (a): where the remaining `env_slot` gap to node lives.** E3 left
  > this as "~5.3 ns that is not slot access and no probe has isolated it". It
  > is isolated now, by a probe that is in the tree —
  > `bench/env_slot_kernel_registers.js`, the same arithmetic and the same
  > checksums with the hot state in bindings no closure captures:
  >
  > | | bronze | node | ratio |
  > |---|---:|---:|---:|
  > | state in an environment record | 11.03 | 5.03 | 2.19× |
  > | state in SSA registers | **4.27** | 3.59 | **1.19×** |
  > | what the record costs | **6.76** | 1.44 | 4.7× |
  >
  > **The gap IS the environment record, and bronze's non-record code is within
  > 19 % of node.** Two more probes say what the record cost is NOT. Made
  > measure-only and unsound on purpose, applied, measured, reverted: giving no
  > value a GC root slot at all is worth **0.24 ns**; dropping the canonicalizing
  > NaN select from every boxed double is worth **0.00**; both together 0.25 of
  > 6.76. E3's hypothesis — the boxing and rooting discipline — is refuted, and
  > so is instruction count, which mechanism 2 above cut 45 % for 0.03 ns. What
  > is left is memory traffic that nothing promotes: same 16 floating-point
  > operations, `__wrapper_render` carries **29 loads, 47 stores and 22 calls**
  > per iteration where the register form carries **3, 7 and 5** and keeps the
  > state in 15 phis. The record is heap-addressable and every call in the loop
  > might reach it, so no slot is ever promoted across the backedge. The next
  > mechanism on this shape is a scoped escape analysis over the record — the
  > slots of a record no call in the loop can name are registers with a
  > write-back, which is what V8 is doing to make the same shape cost it 1.44 ns.
  >
  > **HANDOFF (b): what door 2 (pin enforcement) should know about the store
  > paths.** E1 through E4 have not added a store path; they have made three
  > existing ones narrower and one wider, and a write barrier has to sit in all
  > four. (i) A slot write inside a merged frame region (E3) is a store into the
  > CALLER's frame through a `bronze.frame_region` pointer — the barrier belongs
  > on the slot, not on the frame, or a frameless callee escapes it. (ii) A
  > parameter typed f64 by E4's proof has NO boxed store at all: the value
  > arrives in an f64 register and is written to an f64 slot, so a violation
  > cannot be caught at the store — it must be caught at the CALL SITE, which is
  > fine, because the proof enumerated every call site and door 2 can enumerate
  > exactly the same set. (iii) E4's reach widening turns `EnvSetTdz`/`EnvGetTdz`
  > into plain `EnvSet`/`EnvGet` on a slot the analysis proved unreachable
  > before its initializer; door 2 must not read the absence of a TDZ marker as
  > the absence of a claim — the claim moved into the plan. (iv) The one store
  > path that got WIDER is the definite-init/reach pair itself, and it is the
  > only place in the campaign where a wrong static answer is silent rather than
  > loud: a TDZ read throws today, and a wrongly-elided one reads whatever the
  > slot holds. That is the store path worth enforcing FIRST.
  >
  > **HANDOFF (c): what door 3 (census-driven pin inference) should know.** The
  > closure parameter proof measurably reduces what a manifest has to declare —
  > by five lines of eleven on this campaign's own fixture, and to byte-identical
  > code. A census should therefore run AFTER the proof, not before it, or it
  > will spend its budget proposing pins the compiler already proves and its
  > precision numbers will be inflated by exactly that overlap. What the proof
  > cannot reach is the shape a census is uniquely good at: a closure that
  > ESCAPES — returned from a factory, stored in a field, passed as a callback —
  > where the call sites are real but not enumerable from the declaration's
  > subtree. `param render(iters): number` is one line of the eleven and it is
  > precisely that case. So the division of labour to design for is: the proof
  > owns callee-position closures, the census owns escaped ones, and the census's
  > first job is not "which parameters are numbers" but "which call sites of an
  > escaped closure exist at all" — the same enumeration, gathered dynamically
  > because it cannot be gathered statically.

- **Stage E3 (one root frame per inlined region, and a dead zone nothing can reach)** — 2026-08-25:
  > [!NOTE]
  > **Two mechanisms, and the entry's most useful fact is that they OVERLAP
  > heavily.** `env_slot_kernel` goes **16.78 → 10.77** ns/iter (1.56×),
  > `mat4_kernel` **17.01 → 16.13** ns/call, `call_chain_kernel` 11.38 →
  > **9.38** ns chained with its chained/flat ratio at 0.99. Checksums
  > identical in every column of every row (`126000020` / `12600020`,
  > `400000` / `940000`, `296000000`, `78849652`, `405000`, `-2112298`,
  > `-32601148`, `825756/700159/NaN/-563350`). Against node in the same
  > session (`4.86` ns/iter) the env-slot shape goes from 3.5× to **2.2×**;
  > against node's `14.31` the mat4 shape goes from 1.19× to **1.13×**.
  >
  > Separately the stage fixes a semantics defect E2 found and left: assignment
  > to a `const` binding stored instead of throwing.
  >
  > **A methodology note first, because it changes how the tables below should
  > be read.** The kernel columns here are medians of **101** rounds, not 13.
  > At 13 the shipped configuration is stable to about 1 % but the
  > `BRONZE_NO_FRAME_MERGE` columns are not: three repeats of one four-column
  > spec put the merge-off/definite-off cell at 17.40, 15.23 and 14.92, and
  > the merge-off/definite-on cell at 13.47, 11.02 and 11.13. At 101 rounds
  > two independent repeats of the same spec agree to **0.14 ns on every
  > cell**. The un-merged binary is bigger and its loop touches six frames, so
  > it is the configuration that is sensitive to machine state — which means a
  > 13-round A/B systematically over-reads the merge. Every number in this
  > entry that decides something is a 101-round (kernels), 51-round (ms
  > fixtures) or 41-round (`mat4`, node) median.
  >
  > **1. A merged callee is emitted FRAMELESS** (`fa24b1f`,
  > `src/codegen-llvm/llvm_frame.{h,cpp}`, `llvm_func.cpp`, `llvm_alias.cpp`).
  > E1's direct edges got the callee inlined; they did not get its PROLOGUE
  > inlined. Six inlined bodies in `render`'s loop meant six root-frame
  > allocas, six linkings of the per-thread frame list and six
  > `__bronze_tls_block_cache` fetches, per iteration — all six confirmed
  > in-loop below.
  >
  > A REGION is now planned over the direct-edge graph before any function is
  > emitted: `region(f) = ownSlots(f) + max(region(callee))` over the edges
  > that ask to be inlined. MAX and not sum, because two merged calls in one
  > caller are sequential and never both in flight — the rule the argv region
  > already ran on. An edge that would close a cycle is refused, because a
  > recursive nest has no finite size. The callee's slots are a region of the
  > caller's frame, handed over as a pointer; its view of the thread block is
  > the caller's, handed over beside it. The entry of a merge target becomes a
  > forwarder that allocates the whole region, links it, calls the variant and
  > unlinks — byte for byte the prologue the body used to carry — so an
  > ordinary caller (the uniform wrapper, a refused edge) sees exactly the
  > function it saw before, and the merge is independent of whether the
  > inliner then grants the ask.
  >
  > GC correctness: every value that had a root slot still has one at every
  > safepoint, in a frame that is linked for the whole of the callee's
  > execution and sized for the deepest nest under it. The whole suite is green
  > with the merge on, including every `-gc-stress` variant and the oracle's
  > per-case `BRONZE_GC_STRESS=1` re-run.
  >
  > **Two things the split had to give back, both larger than what it removed
  > until they were.** The measured merge was a **regression** twice before it
  > was a win, and both causes are worth writing down:
  >
  >   - The seam words scattered through the instruction families
  >     (`llvm_arith.cpp`, `llvm_iter.cpp`, `llvm_elem_cache.cpp`,
  >     `llvm_ops.cpp`) each emit a `bronze_tls_block_addr` of their own on the
  >     standing promise that a `readnone` call CSEs with the prologue's. A
  >     frameless variant HAS no prologue fetch, and `cacheTlsFetches` skips a
  >     function whose entry block has none — so every one of them stayed a
  >     real cross-module call, in the loop.
  >   - `tagStackAndControlAccesses` classifies by pointer PROVENANCE, and a
  >     region arriving as a parameter is not an alloca. Without saying what
  >     the two parameters are, the body lost the `StackFrame` claim its frame
  >     accesses used to carry, and every control word it had cached had to be
  >     re-loaded around them. Two parameter attributes (`bronze.frame_region`,
  >     `bronze.tls_block`) and an `Argument` case in `classify()` are the fix.
  >
  > The lesson is that **removing the frames is not what pays; DECLARING what
  > the region pointer is, is.** On the development session's 13-round
  > protocol the merge read as roughly a 1.4 ns regression before the two
  > parameters were labelled and a 2.9 ns win after — so the alias
  > declaration is worth several times the frame and fetch removal it makes
  > possible. There is no seam for it (an unlabelled parameter is not a
  > shippable configuration), so those two figures are development-session
  > numbers on the protocol the honest negatives below discredit; the sign and
  > the ordering are what to take from them.
  >
  > **2. A dead zone nothing can reach carries no marker and no check**
  > (`58d9449`, `ast/queries_declaration.cpp
  > getDefinitelyAssignedLexicalNames`, `lower_scope.cpp`). `let n = 0;` at the
  > top of a scope cannot be read before its initializer: everything ahead of
  > it is a declaration or an inert literal, so no user code can run in the
  > window. Its slot gets no `EnvInitTdz` and its reads emit `EnvGet` instead
  > of `EnvGetTdz`. The analysis walks the statement list and STOPS at the
  > first statement that could call anything (function declarations are
  > transparent); the initializers it accepts are literals and function
  > expressions only. Module-level slots are marked in `planModuleEnv` and not
  > only where the bindings are opened, because a module function that reads
  > one is lowered before `main` exists.
  >
  > The failure mode was chosen. A slot marked definite holds `undefined`
  > rather than the uninitialized singleton, so were the analysis ever wrong a
  > read answers `undefined` instead of throwing — it never reads a marker's
  > bits as a value. Bindings stay lexical in every other respect.
  > `tests/oracle/cases/dead_zone_reachability.js` pins the boundary from the
  > other side: nine programs that must still raise ReferenceError, byte-
  > identical to node.
  >
  > **3. The decomposition, and the overlap.** One interleaved session,
  > medians of 101, warmup discarded, one run of every column per round, every
  > column out of ONE binary (`env_slot_kernel` ns/iter, two-count wall delta
  > over 5.4e6 iterations, checksum `126000020` / `12600020` everywhere):
  >
  > | column | ns/iter | Δ |
  > |---|---:|---:|
  > | E2 (`BRONZE_NO_FRAME_MERGE=1 BRONZE_NO_DEFINITE_INIT=1`, E2 manifest) | 16.78 | — |
  > | + frame merge | 13.88 | −2.90 |
  > | + definite assignment | 11.17 | −2.71 |
  > | + the two new pins (**E3 shipped**) | **10.77** | −0.39 |
  > | E3 shipped, `BRONZE_ELIDE_ENV_GUARDS=1` | 10.66 | −0.12 |
  > | E2, `BRONZE_ELIDE_ENV_GUARDS=1` | 10.29 | — |
  > | the same slots written `var`, so no lexical binding at all | 10.15 | — |
  >
  > That is one path through a 2×2, and the 2×2 is the point. Same session
  > method, the E3 manifest in all four cells, and the whole table reproduced
  > twice to within 0.14 ns:
  >
  > | `env_slot_kernel` ns/iter | definite assignment off | on |
  > |---|---:|---:|
  > | `BRONZE_NO_FRAME_MERGE=1` | 14.99 | 11.12 |
  > | frame merge on | 12.33 | **10.71** |
  >
  > **Alone, the frame merge is worth 2.65 ns and definite assignment 3.86.
  > On top of each other they are worth 0.41 and 1.62. Together they are worth
  > 4.28 — three quarters of what they would be worth if they were
  > independent.** They are not two wins; they are two ways to stop paying for
  > the same thing, a slot access whose guard chain cannot fold into the one
  > before it. E2 named that cost correctly and mispriced its halves in
  > opposite directions: it put the frames at "up to 7.8 ns" and the TDZ
  > merges at 1.72, and the truth is nearer the reverse. **Had definite
  > assignment been built first, the charter's headline item would have
  > measured as 0.41 ns.**
  >
  > The `var` probe says what is left: 10.15 against the shipped 10.77, and a
  > `var` slot differs from a definitely-assigned `let` only in the
  > per-iteration record 14.7.4.9 gives the `for (let i …)` head. E2's
  > "no-new-machinery floor of 9.17 ns" was the pre-registered expectation for
  > this stage; the shipped compiler lands **10.77** against a probe floor
  > that reads 10.15 in the same session, and the 9.17 was a 13-round figure
  > from a session whose absolutes do not transfer.
  >
  > **4. A pinned RETURN is a Number where it is used** (`9cf2756`,
  > `src/types/flow_expr.cpp`). `return <owner>: number` has reached lowering
  > since stage 3.3 and stopped there: the callee returned an f64 and the
  > caller boxed it, because the flow analysis that types the caller's
  > arithmetic never asked the manifest. A closure is exactly the case that
  > needs it — reached through a function value, no `functionIndex`, no
  > signature that can speak for its result. `?.()` is excluded, because a
  > short-circuited optional call produces `undefined`.
  >
  > **The overlap catches this one too.** With `return useProgram: number` and
  > `param render(iters): number` added to `bench/pins/env-slot-kernel.pins`,
  > the shipped fixture reads 10.77 against 11.17 without them — **0.39 ns**.
  > In the un-merged, TDZ-carrying configuration the same two lines are worth
  > **1.79** (16.78 against 14.99), and earlier in this stage, before definite
  > assignment landed, they measured 1.37. In the IL they do exactly what they
  > claim in every configuration: one of the two `bronze_dynamic_add`s goes
  > and `bronze_rel_lt` goes. They stay in the manifest at 0.39 because they
  > are the only coverage the fixture suite has for a `return` pin reaching
  > type inference at all, and the manifest says so in a comment.
  >
  > **5. Assignment to `const` throws now** (`58d9449`). `emitEnvSet` gated the
  > immutable arm on `strictCode_`. The S that 9.1.1.1.5 SetMutableBinding
  > step 7 tests is the BINDING's, not the assigning code's: 14.3.1.1 creates a
  > `const` with `CreateImmutableBinding(name, true)`, so `c = 1` on one is a
  > TypeError whichever mode the assignment sits in. The only immutable binding
  > that takes S from the assigning code is a named function expression's own
  > name (15.2.5, `false`), and that one stores nothing either way; a slot now
  > records which of the two it is rather than a bool. The other half: a
  > `const` no closure reads never becomes an environment slot at all — its
  > value is an SSA register — so `emitEnvSet` never saw it and an assignment
  > was a RENAME. The four writing paths (assignment, compound assignment,
  > update, destructuring) now ask before they store.
  > `tests/oracle/cases/const_reassignment.js` is promoted out of
  > `cases/blocked/` and matches node byte-for-byte, so the oracle suite gains
  > a case and loses a blocked one; the ctest count is unchanged at 29.
  >
  > **6. The elision 2×2, re-measured, and the default does not move.**
  > Definite assignment ON in all eight cells and `--pins` in all eight;
  > `env_slot` medians of 101, `mat4` medians of 41:
  >
  > | `env_slot_kernel` ns/iter | guards on | `BRONZE_ELIDE_ENV_GUARDS=1` |
  > |---|---:|---:|
  > | frame merge on | **10.71** | 10.60 (−0.11) |
  > | `BRONZE_NO_FRAME_MERGE=1` | 11.12 | 10.78 (−0.34) |
  >
  > | `mat4_kernel` ns/call | guards on | `BRONZE_ELIDE_ENV_GUARDS=1` |
  > |---|---:|---:|
  > | frame merge on | **16.13** | 16.18 (+0.04) |
  > | `BRONZE_NO_FRAME_MERGE=1` | 17.06 | 18.44 (+1.38) |
  >
  > **The frame merge repaired the mat4 elision regression — and in doing so
  > removed elision's entire payoff.** E2 measured elision at −6.80 ns on
  > `env_slot` and +1.58 on `mat4`, and the same two cells now read **−0.11**
  > and **+0.04**. That is one fact seen from both sides: what elision was
  > buying was the guard re-derivation the frames forced, and the frames are
  > gone. Note the bottom rows — with the merge refused, elision still buys
  > 0.34 on `env_slot` and still costs 1.38 on `mat4`, exactly the shape E2
  > reported. It is also still not uniform on the wider suite:
  > `typed_array_crunch` −4.55 ms, `mesh_churn_2k` −0.74, `object_graph`
  > −0.45, `three_math` **+0.58**. So the charter's condition ("uniformly ≥ 0,
  > flip the default") is not met — and it is no longer worth meeting. The
  > flag now trades every armed tripwire, and the lowering-bug diagnostic they
  > are, for a tenth of a nanosecond on the kernel it was built for.
  > **Default stays off**, the seam stays as the A/B, and the case for ever
  > flipping it is weaker than it was at E2, not stronger. Whole suite green
  > **29/29 with no flags, 29/29 with `BRONZE_ELIDE_ENV_GUARDS=1`, and 29/29
  > with `BRONZE_NO_FRAME_MERGE=1 BRONZE_NO_DEFINITE_INIT=1`.**
  >
  > **7. IR evidence** (`BRONZE_DUMP_LLVM_IR=<prefix>`, `*.post.ll`,
  > `__wrapper_render`, counted per basic block and split by whether the
  > block's terminator is `unreachable`):
  >
  > | | E2 | + merge | + definite | **E3 ship** | ship + elide |
  > |---|---:|---:|---:|---:|---:|
  > | live instructions | 1387 | 1279 | 437 | **375** | 279 |
  > | live basic blocks | 181 | 170 | 56 | **50** | 37 |
  > | live loads | 211 | 192 | 46 | **35** | 27 |
  > | live stores | 103 | 87 | 54 | **47** | 32 |
  > | live phis | 76 | 74 | 19 | **17** | 15 |
  > | **live calls** | **52** | **37** | **10** | **9** | **9** |
  > | root-frame allocas | **6** | **1** | 1 | **1** | 1 |
  > | `__bronze_tls_block_cache` loads | **6** | **1** | 1 | **1** | 1 |
  > | `bronze_env_get_tdz` | 27 | 27 | **0** | **0** | 0 |
  > | `bronze_dynamic_add` | 2 | 2 | 2 | **1** | 1 |
  > | `bronze_rel_lt` | 1 | 1 | 1 | **0** | 0 |
  > | `bronze_to_int32_f64` | 3 | 3 | 3 | 3 | 3 |
  > | dead (tripwire) blocks | 37 | 37 | 14 | 14 | 0 |
  >
  > All six frames and all six fetches were PER ITERATION, which the loop-body
  > census confirms exactly: of E2's 52 live calls, 50 are inside the loop and
  > they are 27 `bronze_env_get_tdz` + 6 `bronze_tls_block_addr` + 6+6
  > `llvm.lifetime.{start,end}` (the six frames' brackets) + 3
  > `bronze_to_int32_f64` + 1 `bronze_rel_lt` + 1 `bronze_unbox_bool`. The
  > shipped loop carries 7: one fetch, one frame's brackets, the three cold
  > ToInt32 arms, and one `bronze_unbox_bool`. The two dead-block columns are
  > the tripwires: 37 for six frames' worth of guards, 14 for one region's.
  >
  > **8. No regression, and node.** The ms fixtures carry no manifest and are
  > medians of 51; the kernels are as above; node is medians of 41, run
  > manually out of band:
  >
  > | fixture | E2 (both seams) | **E3** | E3 + elide | node v24.2.0 |
  > |---|---:|---:|---:|---:|
  > | `env_slot_kernel` ns/iter | 16.78 | **10.77** | 10.66 | 4.86 |
  > | `mat4_kernel` ns/call | 17.01 | **16.13** | 16.18 | 14.31 |
  > | `call_chain` chained / flat ns | 11.38 / 12.75 (0.89) | **9.38 / 9.50 (0.99)** | — | 2.00 / 1.88 (1.07) |
  > | `nullish_pin_kernel` ns/step | 11.36 | **11.19** | — | — |
  > | `typed_array_crunch` ms | 47.89 | **47.64** | 43.09 | 53.00 |
  > | `three_math` ms | 42.79 | **41.65** | 42.23 | 46.70 |
  > | `mesh_churn_2k` ms | 74.78 | **73.13** | 72.39 | — |
  > | `object_graph` ms | 47.56 | **47.74** | 47.29 | 67.17 |
  >
  > The five no-regression rows are flat or better; `object_graph`'s +0.18 ms
  > is inside its width. Three of the four ms fixtures are now FASTER than
  > node on this box (`typed_array_crunch`, `three_math`, `object_graph`),
  > which is worth saying because the two kernels are where the campaign's
  > remaining gap lives and the wider fixtures are not.
  >
  > **Cross-session drift is again large and again in both directions**:
  > `typed_array_crunch` reads 47.9 here against E2's session's 59.4 on the
  > same source, and `three_math` 42.8 against 33.2. node's `env_slot` reads
  > 4.86 here against 5.08 in E2's session. Only the within-session
  > interleaved columns above are a comparison; the absolutes are not, and
  > stage E4's canonical single-session ladder is still owed.
  >
  > **Commands.** Every column of every table comes out of one build of the
  > compiler; only the environment and the manifest change:
  > ```
  > bronze build bench/env_slot_kernel.js -o env_b.exe --pins bench/pins/env-slot-kernel.pins
  > sed 's/const ITERS = 6000000;/const ITERS = 600000;/' bench/env_slot_kernel.js > small.js
  > bronze build small.js                 -o env_s.exe --pins bench/pins/env-slot-kernel.pins
  > # ns/iter = (median(env_b) - median(env_s)) * 1e6 / 5400000
  > BRONZE_NO_FRAME_MERGE=1 bronze build ...      # refuse every region merge
  > BRONZE_NO_DEFINITE_INIT=1 bronze build ...    # every lexical slot keeps its marker AND its check
  > BRONZE_ELIDE_ENV_GUARDS=1 bronze build ...    # stage 3.4's elision seam, still off by default
  > BRONZE_DUMP_LLVM_IR=<prefix> bronze build ... # the IR evidence above
  > ```
  > The E2 column's manifest is `bench/pins/env-slot-kernel.pins` with stage
  > E3's two additions removed; the rest of the file is unchanged since 3.3.
  >
  > **Semantics and tests.** Two oracle cases.
  > `tests/oracle/cases/const_reassignment.js` (promoted out of `blocked/`)
  > pins the `const` TypeError six ways — uncaptured `a = 2`, captured through
  > an arrow, compound `c += 1`, destructuring `({ d } = …)`, a `for (const e
  > of …)` head, and the same closure inside an explicitly strict function —
  > each of which must throw AND leave the binding at 1.
  > `tests/oracle/cases/dead_zone_reachability.js` is written as PAIRS: the
  > same program twice, once in the shape the analysis proves and once with
  > exactly one statement changed so that user code can run before the
  > initializer. Seven pairs plus a module-level one, covering a closure over
  > a literal-initialized `let`, a binding read by its own initializer's call,
  > both `const` forms, a block's own statement list, a per-iteration `for`
  > head, and the declaration order that makes the prefix stop. The day the
  > analysis widens far enough to swallow the second of any pair, the file
  > stops matching node. Both cases are byte-identical to node under
  > inference, under `--no-infer` and under `BRONZE_GC_STRESS=1`.
  >
  > Five codegen tests assert the region PLAN on a hand-built module — that a
  > chain nests, that two edges from one caller take the max and not the sum,
  > that a cycle is refused, that a self-call is never a region, and that a
  > frameless variant's two parameters carry the storage families they name.
  > Three lowering test files were re-anchored and two tests added: several
  > of their programs opened with exactly the `let x = 0;` shape the new
  > analysis proves, so they were asserting on a check that is now correctly
  > absent, and the anchors were changed to programs that still carry one
  > rather than the assertions weakened.
  >
  > **Honest negatives.**
  > - The headline mechanism is worth **0.41 ns** on top of the residual it
  >   was ranked above. Chartered as "the headline, up to ~7.8 ns", delivered
  >   as one of two heavily overlapping routes to 4.28. The frames were still
  >   the wrong thing to leave in — a per-body prologue in an inlined region
  >   is wrong on its own terms and it is what made `mat4` lose 1.38 ns under
  >   elision — but the ranking that put it first was wrong.
  > - **The 13-round protocol this campaign has used since stage 3 is not
  >   enough for the un-merged configurations**, and it over-reads the merge.
  >   Three 13-round repeats of one spec put a single cell at 17.40, 15.23 and
  >   14.92. Earlier drafts of this entry carried 16.69 → 10.76 with the merge
  >   worth 2.98 and the pins worth 0.01; the 101-round numbers are 16.78 →
  >   10.77 with the merge worth 2.90 and the pins 0.39. The headline barely
  >   moved and two of the attributions inverted.
  > - **Once the guards are elided the merge is worth nothing**: 10.78 → 10.60
  >   on `env_slot`, which is inside the reproducibility band. And E2's fully
  >   seamed configuration plus elision reads 10.29, the lowest single number
  >   this kernel produced all session — a curiosity, not a shipping option,
  >   because it gives up every tripwire to get there.
  > - The `param render(iters)` pin is an INSTANCE fix, not the class fix the
  >   charter asked for. The class question is real and untouched: a closure
  >   has no parameter proof surface, because its call sites are reached
  >   through a function value and nothing enumerates them. And the charter's
  >   premise for that item was wrong twice over — `iters` was never pinned,
  >   and `bronze_rel_lt` already has an inline both-numbers arm, so what the
  >   pin removes is a cold call and a phi, not a compare.
  > - Three cold `bronze_to_int32_f64` per iteration survive, as E2 left them,
  >   and are still fine.
  >
  > **What stage E4 is handed.**
  > - **Use 101 rounds, not 13.** See the second honest negative; the protocol
  >   the campaign inherited is the reason two of this stage's attributions
  >   had to be rewritten.
  > - **The shipped kernel is 10.77 against a `var`-probe floor of 10.15 and
  >   node's 4.86.** The 0.62 between the kernel and the probe is the
  >   per-iteration environment record `for (let i …)` requires (14.7.4.9);
  >   the ~5.3 below that is not slot access at all and no probe in this
  >   campaign has isolated it. The kernel's loop now carries seven calls,
  >   four of them cold arms and one a lifetime marker — there is no call left
  >   to remove.
  > - **A closure has no parameter proof surface.** `param render(iters)` is
  >   in the manifest to work around it. Every sibling-closure call site IS
  >   enumerable once `planStableFunctionSlots` has decided a slot is a stable
  >   function — that plan is exactly a list of the call sites — so this is
  >   buildable and is the general form of what a pin does here by hand.
  > - **`bronze_unbox_bool` and the relational helpers are still opaque.** E2's
  >   lesson (a cold call in a loop is priced as a barrier, not as a call) has
  >   not been applied to them. `bronze_truthy` reads the heap and never runs
  >   user code, so `memory(read)` is available; `bronze_rel_*` cannot be, since
  >   an object operand's `valueOf` is program text.
  > - **Definite assignment stops at the first statement that could call.**
  >   It does not follow control flow, so `let a = 0; if (x) {…} let b = 0;`
  >   proves `a` and gives up on `b`. A dominance-based version over the
  >   lowered CFG would prove both, and would also reach the `let` whose
  >   initializer is a call.
  > - The elision seam is now a 0.11 ns lever on the kernel it was built for
  >   and a −4.55 ms one on `typed_array_crunch`. If it is ever revisited,
  >   `typed_array_crunch` is where the remaining licence lives, not
  >   `env_slot_kernel` — and the question to ask there is why, since that
  >   fixture's env slots should now be as cheap as this one's.

- **Stage E2 (ToInt32 inline, and an access guard that stops merging)** — 2026-08-25:
  > [!NOTE]
  > **Two mechanisms, both of them the removal of a CALL that never needed to
  > be one, and the biggest single stage of the campaign so far.**
  > `env_slot_kernel` goes **48.67 → 16.97** ns/iter (2.87×), `mat4_kernel`
  > **23.55 → 16.86** ns/call (1.40×) — which puts the ladder under the
  > pre-registered ≤ 20 ns target that stage 3.4 concluded was out of reach —
  > `call_chain_kernel` **20.00 → 12.25** ns chained with its chained/flat
  > ratio at **1.01**, and `typed_array_crunch` 64.6 → 58.9 ms. Checksums
  > identical in every column of every row.
  >
  > **1. ToInt32 is emitted inline** (`4098379`,
  > `src/codegen-llvm/llvm_convert.{h,cpp}`). Every bitwise operator, every
  > shift and every integer typed-array store is an ECMA-262 7.1.6 ToInt32,
  > and each was a call to `bronze_to_int32_f64`. The inline form is
  > `dbl >= -2^63 && dbl < 2^63` as a pair of ORDERED compares, then `fptosi`
  > to **i64** and `trunc` to i32.
  >
  > The i64 is the whole design. `fptosi ... to i64` IS the mathematical
  > truncation for every double the range test admits, and truncating that to
  > i32 IS the modulo-2^32 reduction with the signed reinterpretation — so the
  > conversion is two machine operations, and every wrap between 2^31 and 2^63
  > (`4294967296 | 0`, `(2^52+1) | 0`) is an inline answer instead of a call.
  > Bounding at the INT32 edge instead would have been the same two compares
  > for a fraction of the coverage. NaN answers false to both ordered compares
  > and leaves through the same edge as the infinities and anything past
  > int64, all of which keep the helper's fmod reduction; `fptosi` of -0.0 is
  > 0, which is ToInt32(-0) exactly, so negative zero needs no case.
  >
  > **The second half of that commit is the one that was not obvious**:
  > `bronze_to_int32_f64` and `bronze_to_uint8_clamp_f64` are now declared
  > `memory(none) willreturn speculatable`. They are pure functions of one
  > double and always were. Without it the surviving call on the COLD arm is
  > still a memory clobber. Priced on its own (below) it is worth about half
  > of what the whole item is worth, and it is three lines.
  >
  > **2. An access guard's failure edge stops returning** (`3115c75`,
  > `llvm_env.cpp`, `bronze_env_access_failed`). The guard tests three things
  > — object tag, Env brand, slot range — and branched on failure to
  > `bronze_env_get` / `bronze_env_set`, which then branched BACK to a merge.
  > Those helpers `fatal()` on exactly those three conditions, so **that edge
  > has never been able to return**; LLVM simply had no way to be told. So
  > every one of the ~23 slot touches per iteration carried a phi whose second
  > predecessor called an opaque, memory-clobbering external function, and the
  > tag, the brand and the record size were re-derived at every access.
  >
  > The new tripwire is declared noreturn and cold, re-walks the chain only so
  > the diagnostic can name which question failed, and the edge ends in
  > `unreachable`. A non-TDZ environment read is now the LOAD, a write is the
  > STORE, and the TDZ edge is the only one that still merges — correctly,
  > because 9.1.1.1.6 raises a ReferenceError the program can catch and
  > raising in this runtime is a pending cell plus a return, not an unwind.
  >
  > **This is strictly better than eliding the guards**, which is what this
  > stage was chartered to do per-site. Every tripwire stays armed, so the
  > lowering-bug diagnostic `BRONZE_ELIDE_ENV_GUARDS` gives up is kept — and
  > `unreachable` is also what lets a repeated guard on the same record fold
  > into the one before it, because assuming the condition on the way out is
  > exactly the fact a later identical guard needed.
  >
  > **What was NOT built, and why the charter was wrong about it.** Per-site
  > guard elision "driven by the scope plan" has nothing to drive it: the
  > licence is UNIFORM. Depth and index are compile-time constants for every
  > access in the language, and a record's layout is fixed where it is created,
  > so the static plan covers all of them equally — a per-site rule would elide
  > every site, which is the global flag. What varies between fixtures is the
  > PAYOFF, not the licence, and a performance decision is what a flag is for.
  > The tripwire is the version of the same idea that has a static distinction
  > behind it: the claim is not "this guard cannot fail", it is "if it fails,
  > control does not come back", and that one is true at every site.
  >
  > Seams, all three read once per invocation and all leaving the rest of the
  > compiler alone, so every column below comes out of ONE binary:
  > `BRONZE_NO_INLINE_TOINT32=1`, `BRONZE_NO_PURE_CONVERSIONS=1`,
  > `BRONZE_NO_ENV_TRIPWIRE=1` (plus stage 3.4's `BRONZE_ELIDE_ENV_GUARDS=1`).
  >
  > Commands (medians of 13 rounds, one run of every column per round, warmup
  > round discarded, idle box, two-count wall delta for the kernels):
  > ```
  > bronze build bench/env_slot_kernel.js -o env_b.exe --pins bench/pins/env-slot-kernel.pins
  > sed 's/const ITERS = 6000000;/const ITERS = 600000;/' bench/env_slot_kernel.js > small.js
  > bronze build small.js                 -o env_s.exe --pins bench/pins/env-slot-kernel.pins
  > # ns/iter = (median(env_b) - median(env_s)) * 1e6 / 5400000
  > BRONZE_NO_INLINE_TOINT32=1 BRONZE_NO_PURE_CONVERSIONS=1 BRONZE_NO_ENV_TRIPWIRE=1 \
  >     bronze build ...                              # the "E1" column
  > BRONZE_NO_ENV_TRIPWIRE=1 bronze build ...         # the "+ToInt32" column
  > BRONZE_ELIDE_ENV_GUARDS=1 bronze build ...        # the elision seam, still off by default
  > BRONZE_DUMP_LLVM_IR=<prefix> bronze build ...     # the IR evidence below
  > ```
  >
  > | fixture | E1 (all E2 seams off) | + ToInt32 | **E2 shipped** | E2 + elide seam | checksums |
  > |---|---:|---:|---:|---:|---|
  > | `env_slot_kernel` ns/iter | 48.67 | 20.62 | **16.97** | 10.17 | `126000020` / `12600020` |
  > | `mat4_kernel` ns/call | 23.55 | 17.09 | **16.86** | 18.44 | `400000` / `940000` |
  > | `call_chain` chained / flat ns | 20.00 / 18.25 (1.10) | 12.38 / 12.88 (0.96) | **12.25 / 12.13 (1.01)** | 11.25 / 12.38 (0.91) | `296000000 / 296000000` |
  > | `nullish_pin_kernel` ns/step | 12.92 | 11.33 | **11.35** | 11.33 | `825756/700159/NaN/-563350` |
  > | `typed_array_crunch` ms | 64.07 | 64.48 | **59.39** | 56.30 | `78849652` |
  > | `three_math` ms | 33.44 | 35.18 | **33.19** | 34.10 | `405000` |
  > | `mesh_churn_2k` ms | 83.34 | 83.83 | **80.77** | 80.98 | `-2112298` |
  > | `object_graph` ms | 58.34 | 59.64 | **58.29** | 58.76 | `-32601148` |
  >
  > The E1 column reproduces stage E1's shipped compiler to the digit where
  > E1 published a number for it — `call_chain` 20.00 / 18.25 and `mat4` 23.55
  > against 23.60 — which is what licenses reading the rest of the table
  > against that entry. node v24.2.0, same session, same method: `env_slot`
  > **5.08**, `mat4` **14.23** (against its own 14.20 in the campaign summary),
  > `call_chain` 2.00 / 1.88. So `env_slot_kernel` goes from 9.6× node to
  > **3.3×**, and `mat4_kernel` from 1.66× to **1.19×**.
  >
  > The last five rows are the no-regression column and they are all flat or
  > better. Their E1/E2 figures come from a second interleaved session of 25
  > rounds run on the same binaries, because at 13 rounds `three_math` read
  > +0.9 ms and at 25 it reads −0.25: that fixture's width in this harness is
  > about ±1 ms and neither figure is an effect. The `+ToInt32` and elide
  > columns for those five are the 13-round session and carry the same width.
  >
  > **The 2×2 that prices item 1's two halves** (`env_slot_kernel` ns/iter,
  > medians of 13, interleaved, one session; every cell checksum `126000020`):
  >
  > | | helper opaque | helper `memory(none)` |
  > |---|---:|---:|
  > | call | 50.02 | 26.58 |
  > | inline fast path | 21.02 | 20.94 |
  >
  > Read it as **the two overlap almost completely**. Declaring the helper pure
  > is worth 23.4 ns on its own — the call was barely executed and cost half
  > the loop purely as an optimization barrier, which is E1's fourth honest
  > negative confirmed in the other direction. Once the fast path is inline the
  > attribute is worth nothing more (21.02 vs 20.94, inside the width). It
  > ships anyway: it is correct, it is free, it still covers
  > `bronze_to_uint8_clamp_f64`, which has no inline form.
  >
  > **IR evidence** (`BRONZE_DUMP_LLVM_IR`, `env.post.ll`, `__wrapper_render`,
  > counted per basic block and split by whether the block's terminator is
  > `unreachable`, i.e. whether any execution can be in it):
  >
  > | | E1 | + ToInt32 | E2 | E2 + elide |
  > |---|---:|---:|---:|---:|
  > | live instructions | 1281 | 1297 | 1207 | 832 |
  > | live basic blocks | 183 | 192 | 181 | 143 |
  > | live loads | 227 | 227 | 211 | 147 |
  > | live phis | 80 | 83 | 76 | 70 |
  > | **live calls** | **55** | **50** | **40** | **40** |
  > | `bronze_to_int32_f64` | 8 | 3 | 3 | 3 |
  > | `fptosi` | 0 | 3 | 3 | 3 |
  > | `bronze_env_set` | 10 | 10 | **0** | 0 |
  > | `bronze_env_get_tdz` | 27 | 27 | 27 | 27 |
  > | `bronze_tls_block_addr` | 6 | 6 | 6 | 6 |
  > | `bronze_dynamic_add` | 2 | 2 | 2 | 2 |
  > | dead (tripwire) blocks | 0 | 0 | 37 | 0 |
  >
  > The three `fptosi` are the loop's three bitwise ops; the three surviving
  > `bronze_to_int32_f64` are their cold arms. The ten `bronze_env_set` are
  > gone from anywhere control can reach and 37 tripwire blocks appeared in
  > their place, which cost 74 instructions of never-executed code.
  >
  > **Where the remaining 16.97 ns is, measured rather than guessed.** Three
  > probes, all on the E2 compiler, all producing checksum `126000020`, one
  > interleaved session of 13 rounds:
  >
  > | | guards on | guards elided |
  > |---|---:|---:|
  > | the kernel | **16.78** | 10.35 |
  > | the five bodies hand-inlined into the loop in JS | 12.33 | 9.17 |
  > | the same slots written `var`, so no TDZ | 15.06 | 10.38 |
  >
  > 1. **E1's ceiling probe is now wrong, and this is the most important fact
  >    for E3.** E1 concluded "at most ~1.7 ns of what remains is the boundary
  >    … the gap is not the call". Measured today the hand-inlined probe is
  >    **4.45 ns** faster than the kernel. Nothing about the call changed —
  >    what changed is that E1 measured it under the ToInt32 barrier, which was
  >    hiding it. And the 4.45 collapses to **1.18** once the guards are
  >    elided, which says what it actually is: not the call, but the fact that
  >    each of the six inlined bodies keeps ITS OWN GC root frame and reloads
  >    `__env` out of it, so the guard on every access is derived from a
  >    different SSA value and cannot fold into the one before it. The
  >    hand-inlined probe has one frame and one TLS fetch and its guards CSE.
  >    **The root frames are the guard cost.**
  > 2. **TDZ is worth 1.72 ns with the guards on and NOTHING (−0.03) with them
  >    elided.** Definite-assignment analysis over the scope plan was on this
  >    stage's list; it was priced first and not built. The 27
  >    `bronze_env_get_tdz` calls are cold merges, and what they cost is the
  >    guard re-derivation they force — the same cost item 1 above names, paid
  >    twice.
  > 3. **The floor this shape can reach without new machinery is 9.17 ns**
  >    (hand-inlined and guards elided) against node's 5.08. So of the 16.97
  >    the campaign now stands at, roughly 7.8 ns is root frames + guard
  >    re-derivation and roughly 4 ns is something neither probe removes.
  >
  > **Guard elision moved, and is still off by default.** Read strictly within
  > the 13-round session, where its column and the E2 column interleave:
  > `BRONZE_ELIDE_ENV_GUARDS=1` is worth **−6.80** ns/iter on
  > `env_slot_kernel` and **−2.63 ms** on `typed_array_crunch`, and reads
  > −0.44 / −1.04 / −1.79 on `three_math` / `mesh_churn_2k` / `object_graph`,
  > where stage 3.4 measured those three as REGRESSIONS of +0.8 / +1.4 / +1.3.
  > Those three deltas are inside the width the 25-round re-run exposed, so
  > the defensible claim is that elision has **stopped costing** them, not
  > that it now pays on them. The one fixture where it is unambiguously a
  > regression is the campaign's ladder: `mat4_kernel` **16.86 → 18.44
  > (+1.58)**, reproduced in three separate sessions on three separate builds.
  > So the balance has shifted and the conclusion has not: it gives up the
  > tripwires to buy a win it does not deliver uniformly, and the fixture it
  > loses on is the one the campaign is measured by. Whole suite green
  > **29/29 with no flags AND 29/29 with `BRONZE_ELIDE_ENV_GUARDS=1`**.
  >
  > **A defect found and deliberately not fixed.** Assignment to a `const`
  > binding STORES instead of throwing — in sloppy code because
  > `lower_scope.cpp emitEnvSet` gates the immutable arm on `strictCode_`
  > (ECMA-262 14.3.1.1 creates the binding with
  > `CreateImmutableBinding(name, true)`, and that `true` is the S that
  > 9.1.1.1.5 step 4 tests, so the strictness of the assigning code never
  > enters into it), and for an uncaptured `const` because it never becomes an
  > environment slot at all. It is pinned against node in
  > `tests/oracle/cases/blocked/const_reassignment.js`, where the harness fails
  > the day it starts passing. It is not this stage's to fix: a semantics
  > change belongs with its own measurement.
  >
  > Semantics: two new oracle cases.
  > `tests/oracle/cases/int32_conversion_edges.js` pins the boundaries the
  > inline conversion introduces, which `dynamic_int32_wrap.js` never reaches —
  > 2^63 and the largest double below it, -2^63 (inside the fast path), 2^64,
  > 2^52/2^53, the int32 edges, both signs of truncation toward zero,
  > NaN/±Inf/-0, shift counts, and the Int8/Int16/Int32/Uint8Clamped store
  > conversions. `tests/oracle/cases/env_slot_access_edges.js` pins what the
  > guard shape must keep: a closure reading a captured `let` before its
  > initializer runs, the same across a real closure boundary, depth-3 chains,
  > per-iteration records, a `const` dead zone, interleaved reads and writes
  > through two closures over one slot, and a slot whose object value the
  > collector moves under it. Both byte-identical to node under inference,
  > under `--no-infer`, under `BRONZE_GC_STRESS=1` and with every seam flipped.
  > A deterministic 64,000-value differential sweep across sixteen magnitudes
  > agreed between node, the inline path and the helper. Four codegen tests
  > assert the emitted SHAPE on a hand-built module — how many phis an access
  > carries, that every unreachable block's callee is the tripwire, that
  > `bronze_env_get` / `bronze_env_set` are gone from it, and that ToInt32
  > converts through i64 behind two ORDERED compares. Full suite green
  > **29/29**, hot-swap and shared-load included.
  >
  > **What stage E3 is handed.**
  > - **The six GC root frames and six TLS fetches per iteration are now the
  >    top item by a wide margin, and they are bigger than E1 thought.** They
  >    do not merely cost their own stores: they are why 23 accesses to ONE
  >    record derive 23 unrelated header pointers, which is why the guards cost
  >    6.8 ns and the TDZ compares cost 1.7. One frame per inlined region
  >    instead of one per inlined body is worth up to about 7.8 ns of the 16.97
  >    on this kernel, by the two probes above.
  > - Alias scopes per (record, slot) — this stage's charter — would not have
  >    helped and were not built. The env slot loads already carry the
  >    `EnvRecordSlots` family and the record's `flags`/`size` loads are already
  >    `!invariant.load`; what stops them forwarding is that each access starts
  >    from a DIFFERENT `load i64, ptr %gcframe.slot`, which is a value the
  >    collector is genuinely allowed to have changed. Fix the frames and the
  >    aliasing question does not arise.
  > - `bronze_dynamic_add` × 2 per iteration survives (`hits = hits +
  >    useProgram(...)`, and the pinned `return … : number` is still not spent
  >    across a direct edge). Both have an inline both-numbers arm already, so
  >    what is left is the number test and the re-box, not a call.
  > - 27 `bronze_env_get_tdz` cold merges remain. Priced at 1.72 ns today and
  >    at zero once the guards fold, so definite-assignment analysis is worth
  >    doing for its own sake and not for this.
  > - `bronze_to_int32` — the BOXED form, where ToNumber runs first — is
  >    untouched and still a call, correctly: it can run a `valueOf`.
  >    `bronze_to_uint8_clamp_f64` has no inline form either; it is pure now
  >    but still a call, and `Uint8ClampedArray` stores pay for it.

- **Stage E1 (direct call edges to sibling closures, and LLVM inlining across them)** — 2026-08-25:
  > [!NOTE]
  > **The stage did what it was chartered to do, the call boundary on
  > `env_slot_kernel` is now spent, and the kernel moved 11 %.** Both halves of
  > that sentence are the entry. `env_slot_kernel` goes **56.89 → 50.82**
  > ns/iter (checksum `126000020` in every row), and a PROBE that writes the
  > five sibling bodies out by hand in the loop — same captured state, same
  > arithmetic, same checksum — runs at **48.6**. So the edge closed about 6.3
  > ns of an 8.1 ns call gap and there is roughly 1.7 ns of boundary left on
  > this shape. Everything else between here and node's 5.30 is inside the
  > bodies, and most of it is one helper.
  >
  > **What landed: a STATIC CALL PLAN over function-declaration bindings**
  > (`src/lower/lower_scope.cpp planStableFunctionSlots`,
  > `il::Instruction::callEnvHops`). A `function f() {}` written in a scope is
  > the one binding form whose value this compilation knows outright: the
  > declaration IS the value, it is installed before the scope's first statement
  > runs, and — when no assignment anywhere in the scope's whole LEXICAL REACH
  > names `f` — nothing the program can do puts anything else in the slot. The
  > environment that closure captured is the record holding its binding, because
  > a nested declaration is created over the record innermost where it is
  > written. Those two facts together are a direct edge with **no guard at
  > all**, and that is the point: this is the static-plan licence class
  > `llvm_env.cpp` states, the one of the three that owes nothing to the running
  > program. It is a different kind of claim from Stage 3.3's method edge, which
  > is a GUESS spent on a compare — a wrong method target costs a fast path, a
  > wrong target here would be a miscompile.
  >
  > The environment is DERIVED, never loaded: the site hands over the CALLER's
  > own record plus the number of parent links the scope plan counted
  > (`emitEnvAncestor`), so the fast path never reads the closure value, never
  > tests an object tag, and never compares a code pointer. On this kernel the
  > count is **zero** for all five callees — `render` has no record of its own,
  > so its `__env` already IS the record its siblings closed over, and the call
  > forwards a register.
  >
  > **The refusals are the load-bearing half** and every one is name-based and
  > in the safe direction (`ast::getDeeplyAssignedNames`, a walk that crosses
  > nested functions and class bodies, unlike the SSA-sizing walk beside it):
  > a name assigned or re-declared anywhere in the subtree, a name a parameter
  > default writes, a generator or async declaration, any caller or callee
  > inside a machine body, a callee needing an `arguments` object, an over-long
  > call, a `switch` clause's declaration, and a callee not yet lowered when the
  > caller's body was. That last one also makes the edge graph **acyclic** — an
  > edge can only point at a function whose lowering already finished — which is
  > what licenses the unconditional `alwaysinline` ask on it. Self-calls and one
  > direction of a mutually recursive pair therefore keep the dynamic path.
  >
  > **Inlining is asked for at the SITE**, reusing Stage 3.3's `kDirectMethodMD`
  > and its measured 2048-instruction budget, so the boxed path keeps one
  > out-of-line copy of each callee. The ask is made for the CLOSURE edges only:
  > a direct call to a top-level function has been an ordinary LLVM call since
  > the compiler had one, and widening the ask to those is a separate change
  > with its own measurement.
  >
  > `BRONZE_NO_CLOSURE_EDGE=1` is the A/B seam, in the house style of
  > `BRONZE_NO_DIRECT_METHOD`: it refuses every edge and leaves the rest of the
  > compiler alone, so both columns come out of one binary and interleave.
  >
  > Commands (medians of 13 rounds, one run of every column per round, warmup
  > round discarded, idle box):
  > ```
  > bronze build bench/env_slot_kernel.js -o env_b.exe --pins bench/pins/env-slot-kernel.pins
  > sed 's/const ITERS = 6000000;/const ITERS = 600000;/' bench/env_slot_kernel.js > small.js
  > bronze build small.js                 -o env_s.exe --pins bench/pins/env-slot-kernel.pins
  > BRONZE_NO_CLOSURE_EDGE=1 bronze build ...        # the other column
  > # ns/iter = (median(env_b) - median(env_s)) * 1e6 / 5400000
  > BRONZE_DUMP_LLVM_IR=<prefix> bronze build ...    # the IR evidence below
  > ```
  >
  > | fixture | edge OFF | edge ON | delta | checksums |
  > |---|---:|---:|---:|---|
  > | `env_slot_kernel` ns/iter | 56.89 | **50.82** | **−6.07** | `126000020` / `12600020` |
  > | `mat4_kernel` ns/call | 23.60 | 23.72 | +0.12 | `400000` / `940000` |
  > | `call_chain_kernel` chained / flat ns | 20.13 / 18.50 | 20.00 / 18.63 | −0.13 / +0.13 | `296000000 / 296000000` |
  > | `three_math` ms | 21.48 | 22.14 | +0.66 | `405000` |
  > | `mesh_churn_2k` ms | 71.75 | 72.00 | +0.25 | `-2112298` |
  > | `nullish_pin_kernel` ms (raw) | 422.39 | 419.23 | −3.16 | `825756/700159/NaN/-563350` |
  >
  > Only `env_slot_kernel` has a sibling-closure call in it, so only
  > `env_slot_kernel` moves; the other five are the no-regression column and
  > every one of them is inside its run-to-run width. `three_math` is the one to
  > read carefully — it reads +0.66 here and **−0.08** in an earlier session of
  > the same 13 rounds on the same binaries, which is the width and not an
  > effect, and its IL is byte-for-byte identical in both columns because it has
  > no sibling-closure call site at all. Checksums are identical across both
  > columns of every row; the small twin's differs from the big one's by
  > construction (`ITERS` is in the checksum), so they are compared ACROSS
  > columns and never across counts.
  >
  > **IR evidence** (`BRONZE_DUMP_LLVM_IR`, `env.post.ll`, `__wrapper_render`):
  >
  > | | edge OFF | edge ON |
  > |---|---:|---:|
  > | optimized instructions in `__wrapper_render` | 705 | 1463 |
  > | basic blocks | 74 | 183 |
  > | indirect calls through a cached code pointer | 10 | **0** |
  > | `bronze_dynamic_call` slow paths | 5 | **0** |
  > | `bronze_env_get` helper edges | 5 | **0** |
  > | `bronze_to_int32_f64` calls | 8 | 8 |
  > | `bronze_tls_block_addr` fetches | 1 | 6 |
  >
  > The five callee bodies are gone from the loop as call targets and present as
  > inlined regions; `__wrapper_setBlending` (531 optimized instructions),
  > `useProgram` (218), `setDepthFunc` (216), `endFrame` (112) and `total` (277)
  > are all still emitted at exactly their old sizes, which is the out-of-line
  > copy the boxed path keeps.
  >
  > **The honest negatives, and they are the useful part of this entry.** The
  > four figures below come from one interleaved session of 13 rounds that timed
  > the kernel and both probes together; its own `env_slot_kernel` column reads
  > 56.76 / 50.42, which is how it lines up with the table above.
  >
  > 1. **The ceiling probe says the call is nearly spent and the kernel is
  >    still ~10× node.** Hand-inlining the five bodies into the loop in JS
  >    gives **48.71 / 48.53** ns/iter (the two columns of a probe with no
  >    sibling call in it, so they agree, which is the sanity check) against the
  >    edge's 50.42. At most ~1.7 ns of what remains is the boundary. node runs
  >    the same hand-inlined probe at **3.95** and the kernel itself at
  >    **5.30**. The gap is not the call.
  > 2. **`bronze_to_int32_f64` is more than half of this kernel.** A second
  >    probe replacing the loop's three ToInt32-producing bitwise ops
  >    (`i & 7`, `(i >> 1) & 3`, `i & 3`) with f64 counters — a DIFFERENT
  >    checksum by design (`138000003`), so it prices the helper and not the
  >    kernel — runs at **20.31** ns/iter with the edge on and **30.62** with it
  >    off. Three real cross-module calls per iteration are worth roughly 25–30
  >    ns of the 50, and node pays nothing for them (its own probe is 5.08
  >    against 5.30 for the kernel).
  > 3. **Those helper calls are also SUPPRESSING this stage's win.** With them
  >    present the edge is worth 6.34 ns (11 %); with them gone it is worth
  >    **10.31 ns (34 %)**. A call to an opaque external function is a barrier
  >    the inlined bodies cannot be optimized across, so the edge cannot collect
  >    most of what it opened up until ToInt32 is inlined. **This is the single
  >    most important fact for stage E2**: the two items compound, and measuring
  >    either one alone under-reads it.
  > 4. **Access-guard elision and the edge are additive here, and both are
  >    small.** The full 2×2 on this kernel: edge off / guards on **56.76**,
  >    edge on / guards on **50.42**, edge off / `BRONZE_ELIDE_ENV_GUARDS=1`
  >    **51.16**, edge on / elided **47.18**. The guards are re-derived once per
  >    access in each of the five now-inlined bodies, which is why elision keeps
  >    its ~5 ns here; it is still not a uniform win elsewhere and still ships
  >    off by default.
  > 5. **An uncaptured nested declaration gets no edge, and could.** A
  >    `function f(){}` nothing closes over never gets an environment slot at
  >    all — its value stays in SSA — so this plan, which is keyed on slots,
  >    says nothing about it. Every caller of such a binding is in the declaring
  >    body itself, so the record is derivable there too; it was left out
  >    because the ceiling probe says the call is not what is left to win.
  >
  > **What stage E2 is handed.** The loop is now ONE region of 1463 optimized
  > instructions with no calls in it but eight `bronze_to_int32_f64` and the
  > frame bookkeeping. What that region is made of: 183 basic blocks, of which
  > the large majority are `env.ok` / `env.get.slow` / `env.set.slow` — the
  > access guard and its helper edge, re-derived at every one of ~23 slot
  > touches — plus **six** GC root frames and six `bronze_tls_block_addr`
  > fetches per iteration, one for `render` and one for each inlined body, none
  > of which LLVM merged. So E2's list, in the order this stage would rank it:
  > inline ToInt32 (worth ~25–30 ns here, and it unlocks another ~4 of this
  > stage's); merge the per-body root frames and TLS fetches now that the bodies
  > are one function; then the slot traffic itself (alias scopes per slot, the
  > slow-path merges that block GVN, box/unbox folding). Note also that the
  > closure slots are `let`-shaped, so every read carries the TDZ compare and
  > its helper block — visible as `bronze_env_get_tdz` on the cold edges.
  >
  > Semantics: a new oracle case, `tests/oracle/cases/stable_closure_call.js`,
  > pins thirteen of them — a reassigned declaration binding calling the new
  > value, closures escaping the factory, recursion and mutual recursion,
  > two-level hops, shadowing, short and over-long calls, rest parameters,
  > `this`, per-iteration records, block scopes, generators, and a throw across
  > the edge — byte-identical to node on all of it, under inference, under
  > `--no-infer`, and under `BRONZE_GC_STRESS=1`. Plus twelve lowering tests
  > (`tests/lower/lower_stable_closure_call_test.cpp`), most of them about which
  > sites get NO edge. Full suite green **29/29**, hot-swap and shared-load
  > included; a module swap replaces the whole module, so an intra-module direct
  > edge has nothing to say to it.

- **Stage 3.4 (guard elision + integration)** — 2026-08-25:
  > [!NOTE]
  > **The stage was chartered to elide guards and instead found that LLVM was
  > already willing to.** The step-1 diagnosis, read off the IR through the new
  > `BRONZE_DUMP_LLVM_IR=<prefix>` seam (`llvm_backend.cpp`, writes
  > `<prefix>.pre.ll` and `<prefix>.post.ll` around the O3 pipeline), found the
  > inlined `Matrix4.multiplyMatrices` loop in `mat4_kernel` at **808 hot-path
  > instructions for 116 flops** — 65 `fmul` + 51 `fadd`, against 142 loads,
  > 167 GEPs and 28 guard branches over 40 blocks. The 28: eight `pending`
  > exception checks, ~5 direct-method guards, ~10 static-slot receiver
  > guards, 3 element guards, 1 dynamic-add, 1 loop condition.
  >
  > Every one of those eight `pending` loads sat immediately after a store of
  > the form `store i64 %prop.i, ptr %197` — **a GC root slot store into an
  > escaped alloca carrying no alias metadata at all**. The frame is published
  > to the TLS chain, so the alloca escapes, so that store may-aliases every
  > cached control word behind it, so nothing loaded once survived to the next
  > site and no guard over one could be folded into the guard before it. None
  > of the four hypotheses this stage was handed was the cause: the element
  > `head`/`elems` loads were already CSE'd correctly (one `pel.head` and one
  > `pel.elems` feeding sixteen raw loads), and the shape loads' problem was
  > the same clobber, not a missing `!invariant.load`.
  >
  > **The fix is metadata, not elision** (`src/codegen-llvm/llvm_alias.cpp`,
  > new): two storage alias families assigned by POINTER PROVENANCE after the
  > TLS-fetch rewrite — `StackFrame` for allocas, `ModuleTables` for the twelve
  > named `__bronze_*` globals, and the TLS block split **per eight-byte word**,
  > because the frame link and the exception cell are adjacent and after
  > inlining two copies of a body reach them through two phis BasicAA cannot
  > relate. Provenance never walks through an `inttoptr`, which is what keeps
  > the JS heap out of these families; heap accesses keep the emitter's six
  > scopes and merely gain the storage families in their `!noalias`.
  > `BRONZE_NO_STORAGE_ALIAS=1` is the A/B seam.
  >
  > **Kernel (two-count wall delta, 18e6 calls, checksums 400000 / 940000; the
  > four `--pins` rows are the full 2×2 of the stage's two seams, taken
  > interleaved so they are comparable to each other):**
  > - default, no manifest **164.43**
  > - `--pins`, alias off, elision off **25.05** — Stage 3.3's configuration,
  >   reproducing that entry's 25.05 to the digit, which is what licenses
  >   reading the rest of this table against it
  > - `--pins`, alias off, elision on **27.23**
  > - `--pins`, alias on, elision on **25.40**
  > - **`--pins`, shipped (alias on, elision off) 23.52** (23.42 in the
  >   three-column sweep above); node v24.2.0 **14.20**
  >
  > Read the 2×2 as two independent effects, because that is what it is: the
  > alias families are worth **−1.53 ns** with elision off and **−1.83 ns**
  > with it on, and env elision **COSTS +2.18 ns** with the alias families off
  > and **+1.88 ns** with them on. On this kernel the environment guards are
  > not a tax at all — they are loads LLVM has already hoisted out of the loop,
  > and deleting them takes their anchored values with them. That is the reason
  > elision ships as a seam rather than a default.
  >
  > **The measured negatives are worth more than the positive, and they close
  > the ≤ 20 ns question.** Each was a throwaway seam, built to be measured and
  > then deleted; they were taken on the stage's intermediate build, so read the
  > deltas and not the absolutes:
  > - every exception check removed: 23.5 → **22.9** (0.6 ns of 23.5)
  > - NaN canonicalization on stores removed: **no win** (23.9)
  > - GC root frame stores and reloads removed entirely: **no win** (23.7)
  > - environment access guards elided on this kernel: **costs 1.9 ns**
  >
  > So the guards on `mat4_kernel` are worth about 1 ns in total once they stop
  > clobbering each other, and **the pre-registered ≤ 20 ns target is not
  > reachable by guard elision at all**. The third row is the one to carry
  > forward: it prices the GC door that Stage 3.3 left open ("receivers stay
  > boxed because `planRootFrame` only roots `Dynamic`") at approximately zero
  > on this shape. Rooting non-Dynamic slots may still be worth doing for
  > reasons of its own; buying speed is no longer one of them.
  >
  > **Entry-guard-licensed interior elision was NOT built, deliberately.** The
  > two interior guards it would have removed from this kernel are worth
  > ~0.2–0.3 ns by the same measurement. The machinery is a dual-body scheme;
  > the payoff does not exist.
  >
  > **Environment access-guard elision shipped as a seam, OFF by default**
  > (`BRONZE_ELIDE_ENV_GUARDS=1`, `llvm_env.cpp envAccessGuardsElided()`,
  > `emitEnvSlotPtrUnguarded`). It is real — `env_slot_kernel` **56.41 → 51.45**
  > ns/iter (−8.8%, checksum `126000020` both), `typed_array_crunch` 51.67 →
  > **45.42** ms, `three_math` −0.76, `instanced_mesh_churn` −0.76 — but it is
  > not uniformly real: `object_graph` +0.86, `proto_dispatch_churn` +0.47,
  > `mesh_churn_2k` +0.26, and `mat4_kernel` **+1.9**. The guards it removes are
  > lowering-bug TRIPWIRES, not semantics (the rationale block in
  > `llvm_env.cpp` is updated, not deleted), so a default build keeps them and
  > the fixtures that would pay for them are the minority. Output stays
  > byte-identical to the default build on all six fixtures A/B'd above. Full
  > suite green **29/29 with no flags AND 29/29 with
  > `BRONZE_ELIDE_ENV_GUARDS=1`**.
  >
  > **`bench/pins/env-slot-kernel.pins` gains the five signature pins Stage 3.3
  > correctly left out.** That entry measured them as a regression
  > (59.16 → 60.78) because the wrapper's per-argument `ToNumber` bought a win
  > the guarded body could not collect. The alias families changed that
  > arithmetic — the `ToNumber` hoists out of the loop with the rest of the
  > control words — and the same declarations now pay **58.38 → 56.38**
  > ns/iter, reproduced with the columns swapped (58.54 → 56.38), checksum
  > `126000020` in every row. Which way this goes is a property of what else
  > the body costs; it is re-measured, never assumed.
  >
  > **References held**: `nullish_pin_kernel` **12.77** ns/step (was 12.93);
  > `call_chain_kernel` **20.00 / 18.25**, ratio **1.08** (was 21.38 / 18.75,
  > ratio 1.14) against node's 1.00; every pure fixture byte-identical under
  > default, `--pins`, and node. Absolute millisecond figures in this entry
  > come from this box in one session and carry a 5.62 ms process floor; they
  > are not comparable to earlier entries' absolutes, only to each other.

- **Stage 3.3 (typed calling convention)** — 2026-08-24:
  > [!NOTE]
  > **Two declarations and one edge.** `param <owner>(<parameter>): number` and
  > `return <owner>: number` (`src/types/pins.h`) type ONE POSITION of a
  > calling convention f64; the direct method-call edge
  > (`lower_infer.cpp resolveDirectMethodTargets`, `il::Instruction::directTarget`)
  > names the function a `method.call` site will reach so the backend can emit
  > a real call to the typed entry instead of an indirect one through the
  > cache. Neither is worth much alone. Together they are the compounder,
  > because a direct call to a typed entry is an ordinary LLVM call an ordinary
  > LLVM inliner may take.
  >
  > The name is a GUESS and is emitted as one. The guard reuses the site's own
  > inline-cache words — enabled, object tag, PLAIN flags, shape, DIRECT form —
  > and adds exactly one question: is the cached code pointer `@__wrapper_F`
  > for the F this site named? The cache already did the real lookup, so a
  > wrong guess (an override, a subclass, a monkey-patch) simply never matches
  > and the miss block is the boxed path unchanged. That is what makes a
  > GUESSED receiver class admissible where a proof is not available.
  >
  > **Receivers and object arguments stayed BOXED, and this is not a
  > conservatism — it is the GC.** The collector moves; `planRootFrame`'s
  > eligibility is exactly `ty == il::Type::Dynamic` and `forward_value` only
  > rewrites a `Value`. A raw object pointer in a register across an allocating
  > call points into dead from-space. So `this` is a parameter, but a boxed
  > one; the cost is one AND, which is why nothing was lost by refusing it.
  >
  > Commands (medians of 5, warmup discarded, idle box):
  > ```
  > bronze build bench/mat4_kernel.js       -o m.exe --pins bench/pins/threejs-math.pins
  > bronze build bench/call_chain_kernel.js -o c.exe --pins bench/pins/call-chain-kernel.pins
  > BRONZE_NO_DIRECT_METHOD=1        bronze build ...   # edge off
  > BRONZE_DIRECT_INLINE_BUDGET=0    bronze build ...   # edge on, inlining off
  > ```
  >
  > **Call-chain kernel (`bench/call_chain_kernel.js`, three-deep
  > `setValue → same → store`, in-process ns/op over 8e6, `chained` against the
  > hand-inlined `flat` in the same binary; checksums `296000000 / 296000000`
  > in every row including node's):**
  > - nothing: **28.13** / 20.50, ratio **1.38**
  > - direct edge only, no signature pins: **25.00** / 21.00, ratio 1.20
  > - signature pins, edge off: **29.25** / 20.25, ratio **1.46**
  > - signature pins + edge, inlining off: **25.38** / 18.25, ratio 1.38
  > - **shipped (pins + edge + inlining): 21.38 / 18.75, ratio 1.14**
  > - node v24.2.0: 1.88 / 1.88, ratio 1.00
  >
  > Read the third row: **signature pins WITHOUT a direct edge are a small
  > REGRESSION** (28.13 → 29.25). The typed entry is reached through the boxed
  > wrapper, which now pays an inlined ToNumber per argument for a body whose
  > win nothing can collect. Pin a signature where the edge is direct or do not
  > pin it. That is also why `bench/pins/env-slot-kernel.pins` did NOT gain
  > `param setBlending(blending): number` and friends, even though the pin does
  > exactly what it promises there — the IL confirms the compare family
  > collapses from `box.f64` + `strict.eq` + `const.bool` + `cmp.eq` to a
  > single `cmp.ne %1, %5`, an fcmp on two doubles — because `setBlending` is a
  > sibling CLOSURE call with no method site to make direct, and the kernel
  > answers 59.16 → **60.78** ns/iter for it (checksum `126000020` both).
  >
  > **Kernel (`bench/mat4_kernel*.js`, ns/call by the two-count wall delta over
  > 18e6 calls, checksums 400000 / 940000 in every row):**
  > - default, no manifest **182.09**
  > - `--pins`, edge off **27.53**
  > - `--pins`, edge on, inlining off **27.09**
  > - **`--pins`, shipped 25.05**; node v24.2.0 same method **15.7**
  >
  > **The pre-registered ≤ 20 ns target was not reached, and the two rows above
  > it say why the convention alone cannot reach it.** The boundary this stage
  > removes is worth ~0.5 ns (the argument vector) plus ~2.4 ns (the callee
  > prologue: ten callee-saved XMM spills on Windows x64, a GC root frame, a
  > TLS fetch, and every field guard re-derived inside a caller that already
  > established it). The probe's "the remaining ~12 ns is call-boundary cost"
  > was a reasonable guess and is now measured to be wrong: after inlining, the
  > loop's opcode histogram is 233 `movq` / 111 `cmpq` / 173 branches / 70
  > `bzhiq` against 65 `vmulsd` + 51 `vaddsd`. **The residual is guard-dominated,
  > not boundary-dominated** — which is the same answer the env-slot kernel gave
  > in Stage 3.2, now confirmed from the other side. `call_chain_kernel` is the
  > fixture where the boundary IS the work, and there the stage is worth −24%
  > with the ratio moving 1.38 → 1.14 against node's 1.00.
  >
  > The inline budget (2048 IL-lowered instructions, `BRONZE_DIRECT_INLINE_BUDGET`)
  > is measured, not chosen: 1024 refuses `Matrix4.multiplyMatrices` — verified
  > by the surviving `callq __bronze_part$mod1.Matrix4.multiplyMatrices`
  > relocation in `llvm-objdump -dr` output — and leaves the kernel at 27.0.
  >
  > **References held** (same build): `three_math` **20.82 ms** checksum 405000
  > (was 21.78); `mesh_churn_2k` **73.12 ms** checksum **-2112298** (was 74.49);
  > `env_slot_kernel` **59.16 ns/iter** checksum 126000020 (was 59.23);
  > `nullish_pin_kernel` **12.93 ns/step** checksums
  > `825756/700159/NaN/-563350` and `743742/271170/NaN/-453295` (was 13.21).
  > Every pure fixture in this directory also compiles under
  > `--pins bench/pins/threejs-math.pins` with byte-identical output. Suite
  > green with no flags (29/29).

- **Stage 3.2 (nullish-widened pins + env-slot typing)** — 2026-08-24:
  > [!NOTE]
  > Two new declarations, and one of them pays. `--pins ... number-or-nullish`
  > (`src/types/pins.h`) covers the field three.js is full of and the flat
  > lattice cannot type: a slot holding a Number, `null` or `undefined`. It
  > never becomes a lattice element — the read stays `Dynamic` and every boxed
  > consumer of it is the dynamic one it always was — and what it licenses is
  > the COERCING position, where the NaN box makes ToNumber one unsigned
  > compare against the top of the number range plus two constants
  > (`null`→`+0`, `undefined`→`NaN`), branchless. `function <fn>.<binding>:
  > number` pins an ENV SLOT, and beneath it a SOUND greatest-fixpoint proof
  > (`lower_scope.cpp planEnvSlotNumberTypes`) types a captured binding f64
  > with no manifest and no flag whenever every write to it is visible and
  > numeric — which is what makes `n = n + 1` provable, since a single forward
  > pass refuses every counter in the program.
  >
  > Commands (medians of 5, warmup discarded, idle box; the small variant of
  > each kernel is `sed 's/const ITERS = <big>;/const ITERS = <big/10>;/'`):
  > ```
  > bronze build bench/nullish_pin_kernel.js -o k.exe --pins bench/pins/nullish-kernel.pins
  > bronze build bench/env_slot_kernel.js    -o e.exe --pins bench/pins/env-slot-kernel.pins
  > ```
  >
  > **Nullish kernel (`bench/nullish_pin_kernel.js`, ns per `step()` by the
  > two-count wall delta over 4×7.2e6 calls):**
  > - bronze default **43.48**; `--pins` **13.21** (**3.3x**); node v24.2.0
  >   **5.41**. Checksums identical across all three, both counts:
  >   `825756/700159/NaN/-563350` and `743742/271170/NaN/-453295`. The `NaN`
  >   slot is the oracle that matters — the sweep whose slot is `undefined`
  >   must answer NaN, and a read that took the `undefined` singleton's bits
  >   as a double would answer a large finite number instead. A separate
  >   differential run over `typeof` / `=== null` / `??` / `|0` / `String()` /
  >   relational / `-0` / NaN matches node line for line, which is the claim
  >   that the pin left the boxed consumers alone.
  >
  > **Env-slot kernel (`bench/env_slot_kernel.js`, ns per loop iteration over
  > 5.4e6, checksum `126000020` everywhere):**
  > - env typing off (`BRONZE_NO_UNBOXED_FIELDS=1`) **60.54**; sound proof only
  >   (no flags) **59.13**; `--pins` **59.23**; node **4.95**.
  >
  > **That last row is the result, and it is a negative one worth more than the
  > positive one.** Env-slot typing is correct, is never a loss, and buys ~2%.
  > Three measurements say why:
  > - the same kernel with the loop INLINED into the factory (no sibling-closure
  >   calls at all) costs the same — 61.02 / 58.42 / 58.47 — so the call
  >   boundary is not what this shape pays for;
  > - the same kernel rewritten with object FIELDS and `State.*: number` costs
  >   58.40 / 59.71, so there is no env-versus-field gap either;
  > - a loop touching ONE slot costs 3.24 → 2.56 (−21%) with typing, but LLVM
  >   promotes a single slot to a register there, so it is not representative.
  >
  > What is left is ~15 guarded slot accesses per iteration at ~4 ns each, and
  > the guard is the ACCESS, not the value: `emitEnvSlotPtr` (llvm_env.cpp)
  > re-derives the record, tests the object tag, loads and compares the Env
  > brand, loads and compares the record size, and branches to a slow path — at
  > every access — and the lexical form adds a dead-zone compare, plus a whole
  > second guarded read per WRITE (`emitEnvSet` pre-checks the dead zone with an
  > `emitEnvGet` it discards). Typing the slot removes the tag test on top of
  > all that. This is the same lesson `numeric-elements` taught in the other
  > direction: `Matrix4.elements` went 7x because the pin deleted the element
  > ACCESS guard, not because it typed the element.
  >
  > One general fix landed alongside: **`box(bitcast-from-Value)` is now the
  > identity** (`llvm_ops.cpp` `Op::Box`). Only two emitters produce a double by
  > bitcasting a Value — the raw unbox and the pinned plain-array element read —
  > and both carry the claim that the bits are a Number, whose NaN is canonical
  > by construction. Without it a pinned env slot was WORSE than an untyped one
  > wherever the value ends up boxed anyway, which is every `===` against an
  > unproven operand.
  >
  > **References held** (same build, `--pins bench/pins/threejs-math.pins`):
  > `mat4_kernel` **27.75 ns/call** against a 188.97 default (checksums
  > 400000 / 940000); `three_math` **21.78 ms** checksum 405000;
  > `mesh_churn_2k` **74.49 ms** checksum **-2112298**. Suite green with no
  > flags (29/29).

- **Stage 3.1 (pin manifest: the ceiling, kept, on the programs the blanket flag broke)** — 2026-08-24:
  > [!NOTE]
  > **`--pins <file>` replaces the blanket probe with per-(class, field)
  > declarations** (`src/types/pins.h`; manifest at `bench/pins/threejs-math.pins`).
  > A pinned `number` field spends its claim without the builtHere /
  > per-class / write-audit proofs; a pinned `numeric-elements` field reads
  > as an array whose elements compile to raw f64 loads and stores
  > (`il::kElemKindPlainArrayF64`). The mark rides on the TYPE, so
  > `const te = this.elements` keeps it and the method bodies that matter get
  > the raw form. Still a PROMISE, not a proof — enforcement is meant to move
  > to the write paths. `BRONZE_UNSOUND_PINS` remains as the degenerate
  > "pin everything" mode; `--pins` needs no env var.
  >
  > Measured on one build, A/B by manifest, medians of 5 (warmup discarded),
  > idle box:
  > - **Kernel (`bench/mat4_kernel*.js`, ns/call by the two-count wall delta):
  >   27.6 ns/call** — (557.97 − 60.96) ms / 18e6. Identical to the blanket
  >   probe's 27.6, against a 196.9 default; checksums 400000 / 940000.
  > - `three_math` 42.47 → **22.17 ms (−48%)**, checksum 405000. Better than
  >   the blanket probe's 30.02, not worse: the demotion below costs the
  >   blanket flag's wrong `builtHere` claims and buys back what they broke.
  > - `mesh_churn_2k` 77.59 → **73.05 ms**, checksum **−2112298 (correct)**.
  >   The blanket flag produced NaN at 236 ms. That was the whole point of the
  >   chunk, and the fix was not where the probe's note guessed:
  >
  > **The Dynamic-argument skip in method-param joins was the miscompile, not
  > the field pins.** Bisected: a manifest naming only `Vector2.x` — a class
  > `mesh_churn_2k` never touches — still produced NaN, and the field pins with
  > the skip disabled were correct but bought the kernel nothing (193 ns/call,
  > i.e. all of the 7x is that skip). Skipping the contribution outright leaves
  > a parameter typed `builtHere` object on the strength of the call sites the
  > pass agreed to look at, and `builtHere` is the one question a primitive
  > field claim may be spent on — so an ORDINARY audited field read on that
  > parameter becomes a raw unbox of whatever the skipped site passed. The
  > skip now marks the parameter (`MethodInfo::sawSkippedDynamicArg`, sticky)
  > and `widenMethods` demotes it at the fold: the object IDENTITY survives,
  > which is what a shape compare checks and what unlocks
  > `Matrix4.multiplyMatrices`, and every primitive answer drops to `Dynamic`.
  >
  > Full suite green with neither flag set (29/29).

- **Pin ceiling probe (`BRONZE_UNSOUND_PINS`: what pin-based compilation would buy)** — 2026-08-24:
  > [!NOTE]
  > **An UNSOUND, default-off measurement flag, not an optimization.** With
  > `BRONZE_UNSOUND_PINS=1` at compile time, inference spends field-type
  > claims without the builtHere / per-class / write-audit proofs, lets
  > Array/TypedArray field kinds survive a read, skips Dynamic argument
  > contributions in method-param joins (the optimistic stand-in for a
  > census profile), and plain-Array element access on proven receivers
  > compiles to raw dense f64 loads/stores with no guards
  > (`il::kElemKindPlainArrayF64`). It simulates "invariants held by the
  > object model, checks moved to write paths" before that machinery exists.
  >
  > **Kernel (`bench/mat4_kernel.js`, real three.js `Matrix4.multiplyMatrices`,
  > ns/call by two-iteration-count wall delta, idle box):**
  > - bronze default: **196.9 ns/call**; probe: **27.6 ns/call** (**7.1x**);
  >   node v24 same method: **15.7 ns/call**; checksums identical.
  >   Remaining probe-vs-V8 gap is call-boundary cost (boxed IC method
  >   dispatch, boxed args/returns) — the typed-calling-convention work item,
  >   not the body.
  > - Fixture A/B same build (probe off → on): `three_math` 45.68 → 30.02 ms
  >   (**-34%**), `typed_array_loop` 39.41 → 22.08 (**-44%**),
  >   `proto_dispatch` 24.27 → 18.74, `proto_dispatch_churn` 57.27 → 49.29,
  >   checksums matching. `mesh_churn_2k` **breaks** (checksum NaN, 80 → 236
  >   ms): the blunt global flag also pins object-holding arrays
  >   (`Object3D.children`) and audit-refused fields — the exact sites where
  >   real pins need per-field/per-array granularity (census) and write-path
  >   enforcement. That breakage is the probe doing its job: it maps where
  >   enforcement is load-bearing.
  >
  > Full test suite green with the flag unset (29/29); the flag changes
  > nothing unless exported.

- **brobench Chunk 7 (scoped memory alias domains: disjoint heap metadata for LLVM codegen)** — commits `6671fa1`, `9712f58`:
  > [!NOTE]
  > **Expands LLVM scoped alias analysis across 6 disjoint memory domains (`BronzeAliasDomain` in `llvm_alias.h`), licensing LLVM to hoist, reorder, and vectorize across array, object, and header memory accesses.**
  >
  > 1. **Disjoint Heap Memory Domains** (`llvm_alias.h`):
  >    - `TypedArrayData`: element payload bytes of TypedArray views.
  >    - `ArrayElementsData`: JS Array elements slots (`elemsObj->slots[1..]`).
  >    - `ObjectPropertySlots`: inline and overflow property Value slots of plain objects.
  >    - `EnvRecordSlots`: lexical environment record Value slots.
  >    - `TypedArrayViewLength`: view header length word and buffer extbits.
  >    - `ArrayHeaderFields`: JS Array header fields (`length`, `capacity`, `head`, `props`, `elems`).
  > 2. **Metadata Tagging in Codegen**:
  >    - Applied across `llvm_elem.cpp`, `llvm_prop_get.cpp`, `llvm_prop_ic.cpp`, `llvm_prop_set.cpp`, and `llvm_static_slot.cpp`.
  >
  > **Suite Performance (5 runs, Release build, median with warmup discarded vs Node.js v24.2.0 baseline)**:
  > - `three_math.js`: **46.07ms** (infer) vs 46.93ms (no-infer) — **1.05x WIN** vs Node (48.40ms)
  > - `object_graph.js`: **49.75ms** (infer) vs 51.79ms (no-infer) — **1.39x WIN** vs Node (69.38ms)
  > - `typed_array_crunch.js`: **58.73ms** (infer) vs 203.26ms (no-infer) — **0.96x PARITY** vs Node (56.27ms)
  > - `mesh_churn_2k.js`: **88.03ms** (infer) vs 92.67ms (no-infer) — **1.05x WIN** vs Node (92.70ms)
  > - `instanced_mesh_churn.js`: **125.32ms** (infer) vs 135.81ms (no-infer) — **0.76x BEHIND** vs Node (95.28ms)
  > - `fib.js`: **10.90ms** (infer) vs 16.64ms (no-infer) — **3.77x WIN** vs Node (41.06ms)
  > - `numeric_loop.js`: **35.96ms** (infer) vs 93.57ms (no-infer) — **1.82x WIN** vs Node (65.62ms)
  > - `property_access.js`: **10.88ms** (infer) vs 11.54ms (no-infer) — **3.41x WIN** vs Node (37.09ms)
  > - `proto_dispatch.js`: **24.76ms** (infer) vs 26.57ms (no-infer) — **1.46x WIN** vs Node (36.13ms)
  > - `proto_dispatch_churn.js`: **56.15ms** (infer) vs 64.03ms (no-infer) — **0.68x BEHIND** vs Node (38.14ms)
  > - `typed_array_loop.js`: **30.92ms** (infer) vs 47.68ms (no-infer) — **1.32x WIN** vs Node (40.75ms)
  >
  > **Correctness**: 29/29 Release ctest passing cleanly.

- **brobench Chunk 6 (runtime tail knockdown: TLS cache, sort fast-path, Map/WeakMap probe, TypedArray copy, truthy inline, exception inlining)**:
  > [!NOTE]
  > **Addresses the 5.02 ms/frame runtime tail on `many_meshes` identified in Chunk 5's sampler breakdown (Rooted/rooting churn, `tls_block_addr`, sort `mergeRuns`, `unbox_bool`, Map/WeakMap lookup probe, and TypedArray element copies).**
  >
  > 1. **Module-local TLS block cache** (`llvm_abi.cpp`, `cacheTlsFetches`, seam `BRONZE_NO_TLS_CACHE=1`):
  >    Rewrites every used `bronze_tls_block_addr` call into a load from a module-local `thread_local bronze_tls_block*` cache with a once-per-thread miss initialization path, eliminating cross-DLL function calls in function prologues.
  > 2. **`Array.prototype.sort` hoisted-roots merge engine** (`builtin_array_sort.cpp`, seam `BRONZE_NO_SORT_FAST=1`):
  >    Presizes scratch & list buffers to eliminate incremental doubling and GC churn; hoists three `Rooted<Value>` slots (`s.a`, `s.b`, `s.answer`) once for the entire sort instead of constructing/destructing 4 `Rooted` frames per element comparison (~61k times per frame on Three.js render lists).
  > 3. **Allocation-free Map & WeakMap lookup probes** (`map.cpp`, `map.h`, `builtin_map.cpp`, `builtin_weak_map.cpp`, seam `BRONZE_NO_MAP_FAST=1`):
  >    `MapHeader::findFast` probes hash buckets directly without `RootedArgs` or GC frame allocations when index, relocation epoch, and anchor are valid. Accelerates `weakMapGet` (Three.js per-object renderer state) and `mapGet`/`mapHas`.
  > 4. **`%TypedArray%.prototype.set` numeric fast loop** (`builtin_typed_array_methods.cpp`, seam `BRONZE_NO_TA_SET_FAST=1`):
  >    Direct numeric copy loop reading `ArrayHeader::elementsData()` directly into `TypedArrayHeader::set()` without per-element rooting when copying plain array matrix elements into uniform upload buffers.
  > 5. **Inline truthiness in LLVM codegen** (`llvm_ops.cpp`, seam `BRONZE_NO_TRUTHY_INLINE=1`):
  >    Inlines direct bit-pattern checks in LLVM IR for booleans, `undefined`, `null`, the hole, `int32`, and objects (always truthy per ECMA-262), eliminating out-of-line `bronze_unbox_bool` helper calls.
  > 6. **Inlined exception checking** (`exception.h`, `exception.cpp`):
  >    Inlined `rtExceptionPending()` and `rtClearException()` into single-word TLS comparisons against `BRONZE_ABI_NO_EXCEPTION_BITS`, eliminating call overhead after every loop/callback step.
  >
  > **Suite Performance (5 runs, Release build, pure-compute suite vs Node.js v24.2.0 baseline)**:
  > - `three_math.js`: **42.37ms** (infer) vs 48.83ms (no-infer) — **1.14x WIN** vs Node (48.40ms)
  > - `object_graph.js`: **49.49ms** (infer) vs 49.41ms (no-infer) — **1.40x WIN** vs Node (69.38ms)
  > - `typed_array_crunch.js`: **55.94ms** (infer) vs 196.17ms (no-infer) — **1.01x PARITY** vs Node (56.27ms)
  > - `mesh_churn_2k.js`: **81.63ms** (infer) vs 85.67ms (no-infer) — **1.14x WIN** vs Node (92.70ms)
  > - `fib.js`: **9.56ms** (infer) vs 14.97ms (no-infer) — **4.29x WIN** vs Node (41.06ms)
  > - `numeric_loop.js`: **36.99ms** (infer) vs 94.37ms (no-infer) — **1.77x WIN** vs Node (65.62ms)
  > - `property_access.js`: **11.69ms** (infer) vs 10.49ms (no-infer) — **3.17x WIN** vs Node (37.09ms)
  > - `proto_dispatch.js`: **22.71ms** (infer) vs 24.55ms (no-infer) — **1.59x WIN** vs Node (36.13ms)
  > - `typed_array_loop.js`: **28.42ms** (infer) vs 44.82ms (no-infer) — **1.43x WIN** vs Node (40.75ms)
  >
  > **Correctness**: 29/29 Release ctest passing cleanly (oracle, threejs, pixi, gc-stress, threaded-modules, hot-swap).

- **brobench Chunk 5 (observability: sampling profiler + shape-flow census)** — commits `338ec41`, `1a84f5d`, `d8395a3`, `53ab5a6`, `7b4963d`:
  > [!NOTE]
  > **Measurement chunk, no perf claims. Two instruments, then the numbers that decide the
  > monomorphization arc.**
  >
  > 1. **In-process sampling profiler** (`338ec41`, `BRONZE_SAMPLE=1`, default 1 kHz):
  >    runtime-owned Win32 sampler thread — `SuspendThread` + `GetThreadContext` +
  >    `RtlVirtualUnwind` against the JS thread, walk bounded by a module-range map, no
  >    allocation or symbolization during suspension; dbghelp at exit; per-function
  >    self/total tables (full run + `BRONZE_SAMPLE_TAIL_MS` steady-state window) as text +
  >    `bronze-sample-v0` JSON. `1a84f5d` gives it names: `/DEBUG` on exe links and
  >    `BRONZE_EMIT_FN_SYMBOLS=1` promotion of single-object locals to linker publics.
  >    Validation (`d8395a3`): a constructed 90/10 hot/cold split sampled as
  >    **656/73 = 90.0%/10.0%** (and en route pinned LLVM devirtualizing plain-call,
  >    ternary, AND const-array callees — only a spread call defeats it).
  > 2. **Shape-flow census** (`53ab5a6`, `BRONZE_SHAPE_CENSUS=1`): suppresses every latch
  >    (IC fills, absent installs, elem-cache TLS words, method-IC, static publishes,
  >    family stamps — all call-and-continue, so full traffic misses to helpers) and
  >    records per site: receiver Shape\* identity, poly degree, transitions, value-tag
  >    counts. Schema `bronze-shape-census-v0`, documented in `docs/shape-census.md`.
  >    `bench/tools/census_join.mjs` (`7b4963d`) joins both artifacts into the go/no-go.
  >
  > **The go/no-go numbers (fraction of inline compiled-code self time in functions whose
  > property traffic is monomorphic-in-practice, site threshold 99%)**:
  > - `many_meshes`: **98.4%** of 20.15 ms/frame app-code self time (census: 4,692 sites,
  >   151M observations, 94.78% at mono sites). Top self: `Matrix4.multiplyMatrices` 2.69
  >   ms/frame, `setProgram` 1.89, `Vector3.applyMatrix4` 1.23, `projectObject` 1.16,
  >   `painterSortStable` 0.99, `Matrix4.compose` 0.77.
  > - `instanced`: **95.3%** of 7.49 ms/frame (census 93.82% mono). Top: `Matrix4.toArray`
  >   1.58, `compose` 1.40, `__anon_fn_470` 1.33, `Quaternion.setFromEuler` 0.95; ucrtbase
  >   (sin/cos/fmod) is 1.62 ms/frame of the residue.
  > - `hierarchy`: **87.1%** of 2.71 ms/frame (census 70.64% mono) — the polymorphic residue
  >   is the tree walkers seeing every node class: `Object3D.updateMatrixWorld` monoCov
  >   15.4%, `projectObject` 36.3%, `updateMatrix` 16.7%. A family/subtree guard story, not
  >   per-site latching.
  > - `object_graph` micro: **100%** mono (117 sites, 10.2M obs); `searchBFS` 23.8% +
  >   `traverseDFS` 23.0% of samples, 88.5% number-typed loads in the traversals.
  >
  > **Gap attribution, `many_meshes` (sampler tail, ms/frame of ~40)**: app.dll inline code
  > 20.15 (74.2% of self time), runtime DLL 5.02 (18.5%), bronze_host 0.64, nvoglv64 0.62.
  > The runtime 5.02 is a long tail, biggest lines: `Rooted` ctor 0.33,
  > `bronze_tls_block_addr` 0.30, sort `mergeRuns` 0.30, GC `forward_value` 0.29,
  > `rtToNumber` 0.27, `unbox_bool` 0.26, typed-array stores ~0.5 combined —
  > allocation/rooting + re-boxing, not any one helper. `bronze_for_in_keys` is **0.10
  > ms/frame**, so chunk 4's "inline the for-in cache hit" item is dead: skipped on
  > evidence.
  >
  > **Overheads**: env unset, compiled-in instrumentation costs nothing measurable —
  > interleaved A/B (pre-chunk `cbecf92` runtime DLL vs HEAD, same app.dll/exe, paired
  > legs): medians −4.3%/−6.6%/−1.6% (head faster; noise on a fleet-loaded box, min-stat
  > delta +0.3% worst case) — passes the ≤1% gate. Enabled: sampler ≈ +0.5% at 1 kHz;
  > census ≈ **12x/13.7x/11.5x** (mm/in/hi) — a census artifact is counts, so load-immune.
  > This session's machine was never idle (concurrent agent fleet); cross-engine numbers
  > below are same-window interleaved medians of 5, and bro drifted 39.7 → 52.0 ms/frame on
  > `many_meshes` across the evening under fleet load while Chromium held ~6.9, so treat
  > ratios as bounds, not points: bro 51.95/20.35/6.20 vs Chromium 6.93/3.40/1.39 —
  > 7.50x/5.98x/4.45x loaded; the quiet-window bro floor (39.7, matching chunk 4's 39.23)
  > against the same-session Chromium floor (6.9) reproduces chunk 4's ~6x.
  >
  > **Correctness**: 29/29 Release ctest at final HEAD (`BRONZE_WITH_LLVM=ON` verified via
  > `ctest -N`, 853.6s); Debug `bronze_runtime_tests` + `bronze_embed_tests` plain and
  > under `BRONZE_GC_STRESS=1 BRONZE_HEAP_VERIFY=1 BRONZE_GC_POISON=1`; oracle
  > byte-identical vs node. Known and intended: 48 Debug assertions fire if the *tests*
  > run under `BRONZE_SHAPE_CENSUS=1`, because they assert that latches latch and the
  > census's whole mechanism is suppressing latches.
  >
  > **Where the next arc lives**: the census says GO — 94-98% of hot-scene property traffic
  > is monomorphic-in-practice and the hot functions are the three.js math kernels
  > (`Matrix4.*`, `Vector3.*`, `Quaternion.*`), so whole-program shape monomorphization v1
  > should target straight-line kernels with slot-direct loads/stores + number-payload
  > unboxing (object_graph's 88.5% number-typed loads is the unboxing proxy). `hierarchy`'s
  > 70.6% says v1 also needs a *family* guard (one guard per subtree walk, not per site) or
  > the walkers stay helper-bound. Separate follow-up, not monomorphization: the 5 ms/frame
  > runtime tail on `many_meshes` (Rooted/rooting, tls_block_addr, ToNumber/unbox — ABI and
  > allocation shape, addressable without whole-program analysis).

- **brobench Chunk 4 (for-in machinery, overflow-store off-by-one, undef-vs-number rel)** — commits `92f1cad`, `b342560`, `a57cf81`, `b5ee5a2`:
  > [!NOTE]
  > **Four targets from chunk 3's exit profile; three landed, one was premise-false.**
  >
  > 1. **Array named-get "9.28M misses" — premise false.** Measured at HEAD, `object_graph`
  >    makes **74** `bronze_prop_get` calls total (75 with `--no-infer`); the chunk-12 log
  >    entry itself says 132. The chunk-3 exit number came from a stale binary (the MSBuild
  >    static-lib relink staleness class). No code change; documented so the next profile
  >    is taken from a freshly-linked exe.
  > 2. **for-in machinery** (`92f1cad` was the set fix; iter work is `b342560`, cache is
  >    `a57cf81`): (a) Array-kind `iter_step`'s END of walk is now inline (the old inline
  >    path covered only in-range steps, so the LAST step of every loop paid a helper) —
  >    `bronze_iter_step` 1,801,811 → **7** on `many_meshes`; (b) `IterOpen` on an ARRAY
  >    allocates the 56-byte iteration record inline from the LAB (`bronze_iter_open` helper
  >    refills the window when an allocation-free loop starves it) — 1,801,804 → **1,554**;
  >    (c) a shape-keyed enumeration cache in `bronze_for_in_keys` (thread-local
  >    `Shape* → {proto_epoch, arena key list}`; guards: shape identity for own-adds/deletes,
  >    epoch for marked-prototype adds/dict defines/proto swaps, `chainIsCacheable` re-walk
  >    for prototype dict-ification) — call count unchanged at 1,801,804 but the per-call
  >    shape walk + key build is now one probe + arena-key array stamp. Seam
  >    `BRONZE_NO_ENUM_CACHE=1`; `BRONZE_NO_ITER_FAST=1` already covered (a)/(b).
  > 3. **`.group` `bronze_prop_set` site** (`92f1cad`): the inline overflow-store's capacity
  >    guard was off by one — `slotIdx` (= slot − 3) indexes overflow words *including the
  >    block header*, so comparing it against `capacity − 1` refused the LAST slot of every
  >    overflow block, and both the transition arm and the depth-0 hit arm sent that slot to
  >    the helper forever. `many_meshes` `bronze_prop_set` 1,845,677 → **35,639**;
  >    `object_graph` 127,238 → 76,360. Rides the existing `BRONZE_NO_INLINE_OVERFLOW_SET`
  >    seam; oracle case `overflow_last_slot` pins the boundary.
  > 4. **`bronze_rel_gt` 1.80M** (`b5ee5a2`): a symbolized-stack diag showed every call is
  >    `undefined > 0.0` from `WebGLRenderList.push` — three.js asks
  >    `material.transmission > 0.0` on materials with no such property, once per visible
  >    object per frame. New second arm in `emitDynamicRel`: not-both-numbers but each
  >    operand number-or-undefined ⇒ constant false (ToPrimitive is identity on both, so no
  >    user code; NaN loses every ordered compare). null keeps the helper (`null >= 0` is
  >    true); strings/booleans/BigInt/objects keep the helper; valueOf still runs once per
  >    compare. Seam `BRONZE_NO_UNDEF_REL=1` (TLS slot 192); oracle `undef_rel_guards`.
  >    `bronze_rel_gt` fell off the `many_meshes` table entirely.
  >
  > **Helper counts, `many_meshes` (300 frames)**: total 9,724,854 → **2,512,724** (−74%);
  > residual: `for_in_keys` 1,801,804 (71.7%, all cache hits), `has_property` 210,815,
  > `dynamic_add`/`string_concat` 95,806 each, `prop_get` 47,188.
  >
  > **brobench wall (medians of 5, idle machine; this session ran slower than chunk 3's on
  > BOTH engines — Chromium moved 4.27→5.75 / 3.00→3.23 / 1.16→1.33 on the same code — so
  > ratios are the honest cross-session comparison)**:
  > - `many_meshes`: **39.23 ms/frame** (chunk-4 seams off 41.23, so ~2.0 ms/frame — ~5% —
  >   is the same-binary win; Chromium 5.75) — ratio **8.50x → 6.82x**.
  > - `instanced`: 16.63 ms/frame (seams-off 15.68; Chromium 3.23) — ratio 4.90x → 5.15x;
  >   deltas within noise, these mechanisms don't sit in instanced's frame.
  > - `hierarchy`: 5.70 ms/frame (seams-off 5.34; Chromium 1.33) — ratio 4.40x → 4.28x;
  >   within noise.
  > - Suite: `object_graph.js` **60.20 ms** — 1.15x vs pinned Node (69.38), and 1.53x vs
  >   node run live this session (91.86). Checksum stable.
  >
  > **Correctness**: 29/29 Release ctest (`BRONZE_WITH_LLVM=ON` verified via `ctest -N`);
  > Debug `bronze_runtime_tests` 157,564 assertions + `bronze_embed_tests` 330, both plain
  > and under `BRONZE_GC_STRESS=1 BRONZE_HEAP_VERIFY=1 BRONZE_GC_POISON=1`; oracle cases
  > `enum_cache_guards`, `iter_inline_endpoints`, `overflow_last_slot`, `undef_rel_guards`
  > byte-identical vs node with inference, `--no-infer`, per-mechanism seams off, all seams
  > off, and under the GC adversary.
  >
  > **Where the next arc lives**: helpers are now ≤ ~2.5M calls of a ~39 ms frame — well
  > under 1 ms — and the 6.8x gap to Chromium is INLINE COMPILED CODE: nan-boxed property
  > access and re-boxing in three.js's per-object math, exactly the frontier the brass
  > post-mortem named. The one helper line still worth a look is `for_in_keys`' 1.8M
  > cache-HIT calls (inline the probe, or lower for-in over the cached key array without a
  > call). `object_graph`'s residue is `strict_eq` 133,500 + `prop_set` 76,360
  > (`.children`/`.worldZ`/`.y` at 25,440 each — a write-IC refusal class still unnamed).
  > Next chunk should be profile-driven INSIDE compiled code (sampling profiler, then
  > monomorphization / payload unboxing), not helper whack-a-mole.

- **brobench Chunk 3 (String-Keyed Computed Reads: Identity Latch + Arena Keys)** — commits `04e0b51`, `1ae6c03`, `a105585`:
  > [!NOTE]
  > **The bill**: `bronze_elem_get` was 16,254,913 calls/run on `many_meshes` — 55.0% of all
  > 29.54M helper invocations — every one a string-keyed computed read of a plain object
  > (`attributes[name]`, `uniforms[name]`) that HIT the elem cache in C++ but paid the call.
  >
  > **Three commits**:
  > 1. `key_ident` identity latch: ElemCacheEntry grows a word holding the last live string
  >    `equals` proved content-equal to the arena key; the inline string arm confirms with ONE
  >    64-bit compare. GC story: per-collection sweep clears movable-heap idents inside the
  >    pause (arena idents survive); every fill rewrites the ident beside kind/witness/key.
  >    Seam `BRONZE_NO_ELEM_KEY_IC=1`, latch-side. ABI fingerprint moved (entry 48→56).
  > 2. `rtKeyAsValue`: for-in / Object.keys / getOwnPropertyNames / spread hand out the ARENA
  >    shape keys themselves — identity-stable enumeration, ~5M copies/run deleted.
  > 3. `bronze_box_str_key` returns the arena key header: every string literal evaluation is
  >    the same immortal object — no allocation, and string `===` on literals now takes the
  >    inline bit-equality path (`bronze_strict_eq` fell off `many_meshes`' profile entirely,
  >    3.60M → 0 listed).
  >
  > **Helper counts, `many_meshes` (360 frames)**: total 29,542,347 → **9,724,848** (−67%);
  > `bronze_elem_get` 16,254,913 → **37,572** (−99.77%); `bronze_strict_eq` 3,601,170 → gone
  > from the table. `bronze_elem_set` measured immaterial (7,772). `object_graph` checksum and
  > helper profile byte-stable (its bill is named prop_get on array receivers, untouched).
  >
  > **brobench wall (medians of 5, settled idle machine; session ran ~4–10% slower than the
  > chunk-2 session — Chromium moved 3.90→4.27 / 2.88→3.00 / 0.91→1.16 on the same code — so
  > ratios are the honest cross-session comparison)**:
  > - `many_meshes`: 36.28 ms/frame (seam-off 36.20; chunk-2 baseline 34.96, Chromium 4.27) —
  >   ratio **8.96x → 8.50x**. jsMs≈wallMs and BRO_GL_PROFILE puts GL host self-time at
  >   1.9 ms/frame (drawElements 266 ns × 5000), so the frame is inline compiled code, and the
  >   ~0.5 ms/frame the latch buys sits inside ±0.9 noise here.
  > - `instanced`: 14.70 ms/frame (seam-off 14.70; baseline 15.51, Chromium 3.00) — ratio
  >   **5.39x → 4.90x**; the win is commits 2+3 (identity-stable keys, no per-literal allocs).
  > - `hierarchy`: 5.10 ms/frame (seam-off 5.51; baseline 5.04, Chromium 1.16) — the latch is
  >   worth **7.4%** same-binary; ratio **5.54x → 4.40x**.
  >
  > **Suite (this session)**: `object_graph.js` **53.48ms** — 1.30x vs Node (69.38), first WIN
  > on record (was 121.40 at chunk 13, 0.57x); `three_math` 46.55 (1.04x, parity); no checksum
  > moved anywhere; 29/29 ctest, Debug 275/157,564 + 36/330 under GC stress/verify/poison.
  >
  > **Next cost classes, measured**: `many_meshes` residual helpers are the for-in triple
  > (`iter_step`/`iter_open`/`for_in_keys`, 1.80M each = 5.4M, 55% of what remains), the
  > `.group` `bronze_prop_set` site (1.795M — a named write missing its IC), and `rel_gt`
  > (1.80M) — together ~95% of the remaining 9.7M. But the frame itself is now inline compiled
  > code (~36 ms/frame with helpers ≤ ~1.5 ms and GL ~1.9 ms of it): the 8.5x gap to Chromium
  > lives in compiled-JS property access / boxing quality, not in helper traffic.
  > `object_graph`'s gap is 9.28M named `bronze_prop_get` misses on ARRAY receivers
  > (`.length` / `.shift` / `.children`, 48.7% of its helpers) — the named-IC-refuses-arrays
  > class, sized and ready to cut.

- **Chunk 13 (Method-Call IC Miss Closed, Clean Math Property Names & Raw-f64 Field Loads)**:
  > [!NOTE]
  > **Job 0 — CI Fixes & Record Restoration**:
  > 1. Windows debug-info switch updated to `/Z7` (from `/Zi`) in `cmake/bronze_shared_runtime.cmake` to prevent parallel build PDB races.
  > 2. `kIcEntryWords` and `kIcSiteWords` exposed with inline visibility in `src/codegen-llvm/llvm_abi.h`.
  > 3. Recorded missing Chunk 12 benchmarks and historical results into `bench/README.md`.
  >
  > **Job 1 — Runtime Method-Call IC Miss Closed**:
  > Diagnosed the root cause: class methods were created with module environment in `fn->env_record`, failing the runtime IC latch check `(env_record.isUndefined() || ...)`. Implemented environment capture detection (`BRONZE_ABI_FN_FLAG_NEEDS_ENV` and `FunctionHeader::needsEnv()`). Methods without lexical captures now latch ICs directly:
  > - Total Dynamic ABI Helper Invocations on `three_math` plummeted from **1,410,804** to **60,846** (**-95.7%**).
  > - Residual `bronze_call_method` dropped from 450,013 to 27; `bronze_prop_get` dropped from 450,053 to 67.
  > - `three_math` speedup: 44.39ms (infer) vs 122.49ms (`BRONZE_NO_METHOD_CALL_IC=1`, **2.66x faster**).
  > - `instanced_mesh_churn.js` dropped from 319.94ms to 137.85ms (**2.32x faster**).
  > - `mesh_churn_2k.js` dropped from 147.73ms to 101.12ms (**1.46x faster**).
  >
  > **Job 2 — Clean Math Names & Unboxed Raw-f64 Field Access**:
  > Whittled field audit blockers for math properties (`x`, `y`, `z`, `w`, `_x`, `_y`, `_z`, `_w`):
  > - Clean written property names jumped from **138 / 933 (14.8%)** to **175 / 933 (18.8%)** on `three.module.js`.
  > - Proven number field reads increased from **0** to **1,785** sites.
  > - Direct `unbox.f64 ..., raw` loads active in generated IL for Vector3/Matrix4/Quaternion math operations.
  >
  > **Real runs for Chunk 13 (median of 5 runs, warmup discarded)**:
  > - `three_math.js`: **44.39ms** (infer) vs 47.52ms (no-infer) — **1.09x vs Node (48.40ms), WIN** (checksum=405000, down from 92.04ms)
  > - `object_graph.js`: **121.40ms** (infer) vs 155.10ms (no-infer) — **0.57x vs Node (69.38ms)** (checksum=-32601148)
  > - `typed_array_crunch.js`: **57.57ms** (infer) vs 183.50ms (no-infer) — **0.98x vs Node (56.27ms), PARITY** (checksum=78849652)
  > - `mesh_churn_2k.js`: **101.12ms** (infer) vs 106.74ms (no-infer) — **0.92x vs Node (92.70ms)** (checksum=-2112298, down from 147.73ms)
  > - `instanced_mesh_churn.js`: **137.85ms** (infer) vs 148.67ms (no-infer) — **0.69x vs Node (95.28ms)** (checksum=1260786, down from 319.94ms)
  > - `fib.js`: **11.23ms** (infer) vs 15.03ms (no-infer) — **3.66x vs Node (41.06ms), WIN** (checksum=832040)
  > - `numeric_loop.js`: **36.82ms** (infer) vs 96.14ms (no-infer) — **1.78x vs Node (65.62ms), WIN** (checksum=60644102826883.61)
  > - `property_access.js`: **10.53ms** (infer) vs 10.85ms (no-infer) — **3.52x vs Node (37.09ms), WIN** (checksum=3000000)
  > - `proto_dispatch.js`: **47.89ms** (infer) vs 24.73ms (no-infer) — **0.75x vs Node (36.13ms)** (checksum=3000000)
  > - `proto_dispatch_churn.js`: **57.95ms** (infer) vs 61.06ms (no-infer) — **0.66x vs Node (38.14ms)** (checksum=3000000, down from 148.13ms)
  > - `typed_array_loop.js`: **28.86ms** (infer) vs 46.68ms (no-infer) — **1.41x vs Node (40.75ms), WIN** (checksum=523828354.8980187)

- **Chunk 12 (clean math property names, method call ICs, and typed array element fast paths)**:
  > [!NOTE]
  > **Summary of changes delivered in Chunk 12 (Jobs 1, 2, 3)**:
  > - **Job 1**: Clean math property names, method parameter coverage expansion, and file decomposition under 1k lines.
  > - **Job 2**: Method call ICs and call path fast path infrastructure (noting the residual runtime method call miss that will be closed in Chunk 13).
  > - **Job 3**: Typed array and dense array element fast paths in lowering and LLVM codegen, achieving 3.24x speedup on typed_array_crunch (parity with Node.js at 56.29ms vs 56.27ms).
  >
  > **Real runs for commit `de6e5be` (median of 5 runs, warmup discarded)**:
  > - `three_math.js`: 92.04ms (infer) vs 96.90ms (no-infer) — 1.05x (checksum=405000)
  > - `object_graph.js`: 124.59ms (infer) vs 122.93ms (no-infer) — 0.99x (checksum=-32601148)
  > - `typed_array_crunch.js`: 56.29ms (infer) vs 182.38ms (no-infer) — 3.24x (checksum=78849652, 1.00x vs Node 56.27ms)
  > - `mesh_churn_2k.js`: 147.73ms (infer) vs 158.11ms (no-infer) — 1.07x (checksum=-2112298)
  > - `instanced_mesh_churn.js`: 319.94ms (infer) vs 331.35ms (no-infer) — 1.04x (checksum=1260786)
  > - `fib.js`: 10.48ms (infer) vs 15.28ms (no-infer) — 1.46x (checksum=832040)
  > - `numeric_loop.js`: 37.06ms (infer) vs 95.93ms (no-infer) — 2.59x (checksum=60644102826883.61)
  > - `property_access.js`: 10.94ms (infer) vs 11.39ms (no-infer) — 1.04x (checksum=3000000)
  > - `proto_dispatch.js`: 47.26ms (infer) vs 24.74ms (no-infer) — 0.52x (checksum=3000000)
  > - `proto_dispatch_churn.js`: 148.13ms (infer) vs 58.81ms (no-infer) — 0.40x (checksum=3000000)
  > - `typed_array_loop.js`: 27.21ms (infer) vs 46.61ms (no-infer) — 1.71x (checksum=523828354.8980187)

- **Chunk 11 (method-parameter typing: the first clean names — and the honest news that they are the cold ones)**:
  > [!NOTE]
  > **The audit's first nonzero verdict.** Method parameters join over call sites where the receiver's class set is proven (`src/types/method_ident.{h,cpp}`, seam `BRONZE_NO_METHOD_PARAM_TYPES` — OFF reproduces the chunk-10 IL hash `0a431f46…` byte-identically). 188 of 1,222 methods (15.4%) keep parameters; combined with the computed-write whittling (289 → 166 sites; receiver-not-built refusals 74 → 0), **48 of 938 names come clean and 21 raw-f64 field loads go live** — the IL md5 finally flips under `BRONZE_NO_UNBOXED_FIELDS` on three.js itself, not just oracles. The 48 are counters and lengths (`_lodMax`, `referenceCount`, `spotLength`, …), not `x/y/z/w`: the math names still fall to the 84.6% of methods that lose their parameters, the 166 surviving computed writes, and 38 class-layout refusals (a `LineSegments` base accounts for 7). **Bench: honestly neutral** — interleaved ON/OFF medians 44.5/41.7, 16.7/16.4, 6.2/5.5, no separation beyond noise, exactly what cold clean names predict. (The delivery's own bench table compared against `BRONZE_NO_INLINE_CALL`/`BRONZE_NO_ELEM_IC` — older mechanisms — and its +24–45% claims are misattributed; rejected on validation, replaced with the seam-correct A/B above.)
  > Also landed and verified: `Reflect.construct` (full 28.1.2), callable-Proxy sort comparators, coercible `String.prototype.concat`, each with paired oracle cases; adaptive GC heap growth (1 GB reservation, 2×-survivor threshold, 8 MB headroom floor) — hierarchy depth 7 (21,844 nodes) survives all 300 frames, unpinning the bench scene. Full suite 29/29 including 550 s pixi under GC stress.

- **Chunk 10 (constructor-parameter typing: the join becomes a proof, and the audit names its next two blockers)**:
  > [!NOTE]
  > **The mechanism** (`src/types/ctor_ident.{h,cpp}`, seam `BRONZE_NO_CTOR_PARAM_TYPES`): constructor parameters are typed by joining over every construction site — `new C(...)`, `super(...)`, and parameter defaults (a bare `new Vec()` binds `constructor(x = 0)`'s default, contributing Number). Unlike the method join, `new C` *names* C, so the join is a proof and licenses primitives and unboxed f64. Poison is per-class and sticky, covers ancestors (because `Object.getPrototypeOf` walks a subclass constructor to its base's), and the chunk-7 trap is closed the right way: `new Curves[lineType]()` costs only the classes in that table, `new <recv>.constructor(a)` only the receiver's subtree. On three.js: **152 of 211 constructors keep their parameters (72%)** over 218 classes; of 261 computed-new sites, 31 bounded by receiver, 202 reach nothing, 30 reach every class (dominated by ~9 `new source.array.constructor(...)` typed-array clones).
  > **The honest chain result: links 2–4 did not move on three.js.** Clean names stay 0 of 938, raw loads 0, IL md5 identical under `BRONZE_NO_UNBOXED_FIELDS` — because `numberClean()` is blocked by two *whole-program* refusals this chunk never touched (computed writes with unproven key **and** value: 289 sites; computed delete: 23) and, independently, by method parameters being `objectIdentityOnly`: one `set(x, y) { this.x = x; }` refutes the name even with every constructor fully typed. **Method-parameter typing is the next lever, and the two whole-program refusals ride along.** On the oracle cases the chain completes end-to-end: md5 flips under the seam on 5 of 6 (the 6th feeds string concat, so no rawUnbox — by design).
  > **Bench: neutral, as link 3 predicts.** 3 interleaved ON/OFF passes per scene: many_meshes 44.2/42.8, instanced 18.2/17.6, hierarchy 6.5/6.3 (medians, ON/OFF) with per-sample spread wider than every gap. The machine read slower than the chunk-9 baseline in absolute terms that day (the OFF build *is* the pre-chunk compiler byte-for-byte and it measured 42.8/17.6/6.3) — machine state, not regression; the ON-vs-OFF comparison is the measurement. 29/29 ctest, 46/46 types doctests, pin matrix byte-identical across all three seams (`BRONZE_NO_CTOR_PARAM_TYPES BRONZE_NO_UNBOXED_FIELDS BRONZE_NO_VALUE_FLOW`), GC-stress included. Found broken along the way: `Reflect.construct` unimplemented (reported); a positional-argument hazard in `analyzeFunction` that had silently disabled chunk 9's value flow (43,134 IL lines moved when fixed); a sticky over-refutation from round-1 `Never` field types (fixed, seam-neutral).

- **Chunk 9 (clone origins: the facts every constructor was silently losing — plus module-binding value flow and a computed-key audit that stops lying)**:
  > [!NOTE]
  > **The find that outranked both briefed jobs.** `lowerClass` copies constructor bodies with `ast::cloneStmt`, and inference keys its facts on *node identity* — so every fact about every statement inside every one of three.js's 200 constructors (f64 loop-header block params, pristine-Math sites, static slots, certified field reads) was looked up under a pointer inference had never seen, and silently defaulted to Dynamic, since constructors were first lowered this way. `ast::CloneOrigins` maps each clone to its original; every inference query in lowering walks the chain (a class inside a constructor is a copy of a copy). Field-*initializer* copies deliberately carry no origins — inference walks them in class-declaration scope, lowering evaluates them in the constructor, and `let k=1; class C { f=k; } k="s"` pins why transferring facts across that boundary would be unsound. **Native property accesses 28.5% → 36.0%** (static-slot 4,135 → 5,465, family-guarded 1,113 → 1,507), almost all from this fix.
  > **Module-binding value flow** (seam `BRONZE_NO_VALUE_FLOW=1`, compile-time): top-level bindings harvest a joined type inside the call-graph fixpoint; `lookup` answers `objectIdentityOnly` — never a cell, never an unboxed value. Bench-neutral today; it is the substrate the constructor-parameter chunk converts. The locally-known-function-value join was *measured, then declined*: of 767 bare-identifier call sites, only ~59 argument positions carry a proven class — the mechanism would have had nothing to eat. The 9,587-site `identifier` row is now six named, counted rows (local 4,017, function-value param 2,761, captured 1,547, global/host 709, module binding 15, parameter+reason), and `receiver is dynamic: this` fell 1,546 → 190.
  > **The audit's computed-site count was lying, and now it is not.** 2,601 was 451 sites recounted every fixpoint round, with round-1 `Never` types treated as refutations. Truth: **451 sites, 139 proven harmless** (Number key or Number value), refutations sticky and per-site; `delete o[k]` goes through the same typed path instead of an unconditional global refusal. Clean names stay **0 of 938** for one named reason: 72 of 73 number-field reads are refused *receiver not built here*, because `method_ident` excludes constructors and the math classes assign `x/y/z/w` from constructor parameters. That constraint is the whole of the next chunk.
  > **Bench:** `instanced` −1.7% (every one of 10 interleaved baseline samples above every new sample), many_meshes and hierarchy inside noise. `BRONZE_NO_UNBOXED_FIELDS` needs no timing: the IL is md5-identical both ways while clean names are 0. 29/29 ctest; three new oracle cases (ctor-body facts, module-binding flow with TDZ/var-shadow edges, computed keys incl. `q[1]` vs `q["1"]` vs `q["01"]`) byte-identical across 108 pin-matrix rows. `--infer-stats` now works on `bronze il` (6 s, vs 7 min through LLVM emission) — the measurement loop is usable again.

- **Chunk 8 (field types: the harvest becomes a proof, the unbox becomes a bitcast — and the audit that says exactly why it never fires on three.js)**:
  > [!NOTE]
  > **Job 1 — the miscompile, closed at its root (unseamed, mandatory).** `ClassLayout::fieldTypes` was a *harvest* — the join over every `this.<f> = ...` a class body writes — spent as a *proof*: it reached `mergeParamType`, made a loop header's block parameter `f64`, and hard-unboxed `v.x = "hi"` into `NaN`. The write it never saw was three lines from the class. `types/field_audit.{h,cpp}` is the pass that earns the proof: the unit is the property NAME, not the class — a write through any unproven receiver can land on any instance, so no class-scoped audit can speak for it — and the invariant is name-global and coinductive: *every property named `f` anywhere in the heap holds a Number*, established by the constructor's write and preserved by every write the program contains, or refuted. Poison is monotone (clean → refuted only) and runs inside the inference fixpoint, because the type of `this.x = v.x` depends on whether `x` is clean, which depends on that very write. A computed write refutes *every* name unless the flow pass proves its key a Number (a numeric key reaches only canonical-numeric names — what keeps `array[i] = obj` from refuting `x`, `y`, `z`) or its value a Number. Builtin-owned names (`length`, `flags`) are refused before any write is read. Oracle case `field_type_audit` pins the original miscompile shape: it now prints `hi`, not `NaN`.
  > **Job 2 — the raw load is ONE INSTRUCTION.** bronze's Value is NaN-boxed with the number range at the *bottom* of the encoding, so a Number's 64 bits are exactly its double's 64 bits: a slot proven to hold nothing but Numbers already *is* raw f64 storage, and the collector already walks past it as a non-pointer. Nothing about representation, allocation, or the GC contract changes; a site carrying the proof simply stops paying for the tag test (`inst.rawUnbox` → a bare `bitcast` in `llvm_ops.cpp`). Seam: `BRONZE_NO_UNBOXED_FIELDS=1` (compile-time). Oracle case `unboxed_field`; both cases byte-identical across infer/no-infer, the seam built in and out, GC stress, and heap-verify+poison — 28 matrix rows per case, all clean. 29/29 ctest, pixi under stress included.
  > **The instrument's verdict, which is the real product.** `--infer-stats` now prints the audit's report, and on three.js it reads: **0 of 938 written property names hold only numbers**, because a *global* refusal stands — **2,601 computed-write sites whose key and value are both unproven**, plus 23 computed deletes. Even locally, only 45 names carry no refusal of their own (394 written as dynamic, 178 as bool, 95 as function...). Of the 73 number-field read sites that asked for the raw load, **72 were refused "receiver not built here"** and 0 by the write audit. So the mechanism is sound, wired, and fires approximately never on this library — and the report ranks what that costs: prove computed KEYS (loop indices the flow pass already types) to lift the global refusal, and carry receiver identity through values (the 9,582-site row from chunk 7's histogram) to license the sites. Both are the same next chunk.
  > **Benchmark: flat, and honestly so** (rebuilt bro-tree compiler — the tree's `bronze.exe` is `EXCLUDE_FROM_ALL` and a mid-chunk stale snapshot first measured a 6 ms regression that did not exist): `many_meshes` **37.0–37.3** (was 36.7), `instanced` **15.6** (was 15.2), `hierarchy` **5.3** (was 5.3), all within run-to-run spread. A correctness chunk that withdraws an unsound speedup and rebuilds it on a proof owes the suite parity, and pays exactly that.

- **Chunk 7 (layout families, and interprocedural identity): the 952 sites chunk 6 gave back, claimed by a guard that recognises a whole `extends` subtree — and the instrument that says where the remaining 11,694 identifier receivers actually live**:
  > [!NOTE]
  > **Job 1 — a different guard, not a weaker claim.** Chunk 6's 952 declined sites are all one shape: `this` inside a method of a class somebody extends. The *layout* was never wrong there — a subclass runs `super()` before installing anything of its own, so its properties begin with its base's, at the same slots — what failed is that a cell pins one shape and `Object3D.updateMatrixWorld` runs on Groups, Meshes and Scenes. So the runtime now stamps each **shape** (`Shape::family_stamp`, `runtime/class_family.cpp`) with the most specific registered class whose whole declared field list it verified as a prefix of that shape — **name by name, slot by slot, attribute by attribute** — and a site asks whether the stamp names a class in its own `extends` subtree. Preorder interval labelling over the proven-class forest makes that `stamp - (base + lo) <=u span`: one load of an immortal shared word that is already hot, a subtract, an unsigned compare — and it *removes* the per-site cell load the identity form pays. Seam: `BRONZE_NO_FAMILY_GUARD=1`.
  > - **The invariant is verified, not assumed.** `ClassLayoutTable::resolve` refuses any class whose fields are not a genuine prefix of its base's, attributes included, so a family range can never span a class whose slot N holds a different property. And because the *runtime* re-checks the prefix against the real shape, a subclass the compiler never modelled — an anonymous `class extends Mid` returned from a function — is **handled** rather than refused: its shape verifies, gets stamped, and the inherited sites hit it.
  > - **Why one write per shape is enough, forever.** Every way a property's name, slot or attributes can change under a live object either transitions it to a different shape or drops it into dictionary mode: `delete`, `Object.freeze`, a `writable: true → false` redefinition, `setPrototypeOf`. All four route through `toDictionary`, which mints a private unshared shape — and a dictionary shape is stamped `FAMILY_NONE`, never a family.
  > - **Result: 3,022 → 3,974 static-slot sites (+952, exactly the declined set)**, over **62 layout-family roots spanning 195 of three.js's 200 proven classes**. Six doctests (`tests/runtime/class_family_test.cpp`) ask the registry directly, because no oracle case can observe *which* path produced a correct answer.
  >
  > **Job 2 — a method's callers, enumerated through the RECEIVER.** A method is reached through a value, so its callers cannot be enumerated by name the way a module-level function's can. They *can* be enumerated by the receiver's class, and `types/method_ident.{h,cpp}` does that: the arguments at every `<receiver of proven class C>.m(...)` join into every method that dispatch can reach — the nearest declaration at or above C, plus **every override in C's subtree**, because a `C`-typed receiver is usually a subclass. Poison is monotone, sticky and named: a call on a receiver whose class is not proven, a name read as a value, `.call/.apply/.bind`, a spread at a call site, a non-plain parameter list, and a computed call whose property name is not a fixed set of literals (that one poisons every method in the program). Seam: `BRONZE_NO_INTERPROC_IDENT=1`.
  > - **A parameter identity is a GUESS, and the type system now says so.** `Type::objectIdentityOnly` marks an identity that came from a join over call sites rather than from watching an object being built. It licenses exactly what an object identity has always licensed — the guarded property-site form — and is *forbidden* the one road from an identity to a value: `fieldTypeOf` answers `number` for a field the class body only ever assigns numbers to, `Number` licenses unboxed f64, and no guess may license that. A field read through a guessed base keeps the (still guessed) identity and drops every primitive answer to `Dynamic`.
  > - **Two blunt rules that were measurably wrong**, both found by reading the poison list rather than the totals. `super.copy(source)` is a **call**: reading it as an escape poisoned `copy`, `clone`, `dispose`, `toJSON`, `updateMatrixWorld` and thirty more of three.js's workhorse names. And a read that is **consumed where it stands** — `i < array.length`, `box.min.x`, `!material.onBeforeCompile` — cannot hand a function to anyone; `length`, `min`, `max`, `center`, `scale` and `distance` are method names on the math classes and property names on half the library, and that collision was all the escape rule was reporting. 95 → 57 poisoned names; the "read as a value" row fell from 985 sites to 35.
  > - **Result: 6,441 → 6,614 native property accesses (27.8% → 28.5%), +161 static-slot sites, identifier receivers 11,865 → 11,694.** Small — and the instrument built for it says why.
  >
  > **The measurement that changed the design, again.** Letting a guessed identity claim a **cell** is slower than not claiming the site at all, and the scene that says so is `hierarchy`: **5.27 → 5.72 ms** with cells allowed (+8.5%, against 5.58 with the whole chunk switched off), `instanced` 15.35 → 15.53, `many_meshes` inside noise. Same shape as chunk 6's 952 sites and the same cause — a cell pins one shape, and a parameter is precisely the receiver that is *not* one shape. So a guessed identity may claim a slot only where the guard tolerates a family, and takes the ordinary inline cache otherwise. All 161 sites survived that restriction as family sites (family-guarded **952 → 1,113**). The probe is a one-line temporary seam in `claimStaticSlot`, measured and removed; the numbers above are the re-measurement described under *measurement hygiene*.
  >
  > **Same-compiler seam A/B** (every seam here is compile-time, so one bro binary and four `app.dll`s; 300 frames after 60 warmup, interleaved passes in opposite order, machine otherwise idle, ms/frame):
  >
  > | scene | all off | chunk 6 only | + job 1 | + job 2 (shipped) |
  > |---|---:|---:|---:|---:|
  > | `many_meshes` | 37.24 | 36.76 | 36.92 | **36.65** |
  > | `instanced` | 16.51 | 15.45 | 15.35 | **15.24** |
  > | `hierarchy` | 5.58 | 5.61 | 5.31 | **5.30** |
  >
  > Read the `hierarchy` row: chunk 6 alone is **5.58 → 5.61**, exactly the wash it reported after declining the 952 sites, and Job 1 turns that into **5.31** (−5.4%). `instanced` is −7.7% end to end, most of it chunk 6's. `many_meshes` moves about −1.5% with a ±0.6 ms run-to-run spread, so its per-job split is not resolvable and is not claimed. Job 2 is inside noise on all three, as a +173-site coverage move on a 23,000-site file should be. Confirmation run with the final compiler, two more interleaved passes: **37.27 → 37.03**, **16.43 → 15.23**, **5.62 → 5.33**. `bench/run_benchmarks.sh --runs 5` both ways, every checksum identical: `three_math` **47.51 → 46.20** (PARITY → WIN), `instanced_mesh_churn` 137.66 → 127.55 (−7.3%), `mesh_churn_2k` 88.07 → 83.72 (−4.9%), `typed_array_loop` 28.50 → 27.06, `proto_dispatch_churn` 60.56 → 59.64, nothing outside noise the other way.
  >
  > **Measurement hygiene, learned the expensive way.** `many_meshes` is violently sensitive to *any* other work on the machine: with a single concurrent `ctest -R pixi` — one bronze compile, on 32 idle cores — the same `app.dll` measures **67–73 ms instead of 36.8**, while `hierarchy` moves 5.2 → 5.36 and `instanced` 15.5 → 15.8. Chunk 7's first A/B round was taken with a full `ctest` running beside it and reported a regression that did not exist, and a validation pass taken the same way reported a 21 ms one. The scene is 5,000 draw calls a frame, so it depends on the driver's worker threads getting scheduled promptly, and one busy core is enough to take that away. **Bench this suite on an idle machine, or do not quote the number.**
  >
  > **The finding worth keeping.** `--infer-stats` now splits the identifier row by *why the parameter was refused*, and the answer reframes the target chunk 6 named: **only ~2,100 of the 11,694 identifier receivers are method parameters at all.** The other **9,582** are locals and the parameters of ordinary functions — three.js's renderer is large factory functions (`WebGLTextures`, `WebGLState`, `WebGLBackground`) full of nested function *expressions*, reached through values, whose callers neither name-based nor receiver-based enumeration can see. Of the parameters, 1,414 are held by "called on a receiver whose class is not proven"; relaxing that rule was measured at **+145 further sites**, which is not the trade chunk 6 already paid for. Full histogram: `brobench/analysis/chunk7_infer_histogram.md`.
  >
  > **A pre-existing crash this chunk found and fixed.** `oracle-pixi` fails under `BRONZE_GC_STRESS=1` at chunk 6's HEAD (`99daff6`) — verified against a pristine worktree at that commit — with an access violation and no output, and it goes away under `BRONZE_NO_STATIC_SHAPES=1`. The one-shot publish behind a static site's fallback call read the receiver from the register the guard was built from; that call allocates, a collection inside it moves the receiver and writes the new address into the GC root frame, and the publish then dereferenced the stale one for the flags byte and the shape word. It now reloads the receiver from its root slot (`FunctionEmitter::rootSlotAddrOf`) and declines to publish when the value has no slot. Site counts unchanged; **29/29 ctest green, pixi included, for the first time under stress.**
  >
  > **Honest scope.** Still boxed slots only — `ClassLayout::fieldTypes` remains unconsumed and the GC contract is untouched. Constructors are deliberately outside Job 2: three.js's four `new Curves[type]()` sites would cost every constructor in the program its parameters. Two oracle cases pin the new edges — `interproc_ident` (subtree dispatch, `super`, `.call` with a foreign receiver, spread/rest/default, recursion, a return through a conditional, a poisoning call site late in the module, an unmodelled anonymous subclass, an instance that gains a property) and `interproc_ident_dynamic` (the computed call that poisons the program) — byte-identical across infer/no-infer, three compile-time seams and their combination, three run-time seams, GC stress, and heap-verify+poison: **240 configurations over the chunk's six cases, all clean**.

- **Chunk 6 (static shapes by proof): class layouts proven whole-program, fixed-offset loads behind a shape compare — and the diagnostic that said the old coverage number was inert**:
  > [!NOTE]
  > **Diagnose first.** The brief asked for a failure-reason histogram before any design, and it earned its place twice over. Splitting the old single `receiver is dynamic` bucket by the *syntactic form* of the receiver (`lower_infer.cpp`, `dynamicReceiverForm`) put 11,926 identifier receivers at the top and 6,382 `this` receivers second — but the four leading rows turned out to be one causal chain, not four problems: ES6 `class` declarations never entered the constructor index, so `new Vector3()` typed as *an object with no identity*; `this` was unconditionally `Dynamic`; so every field read was `Dynamic`; so every local holding one was too. The second finding was worse and reshaped the chunk: `llvm_prop_get.cpp` and `llvm_prop_set.cpp` both opened with `(void)monomorphic;`. **The pre-existing "2.1% native property access" statistic was inert** — a site counted native emitted the same inline-cache sequence as one counted dynamic. Raising it alone would have bought nothing, so coverage and a codegen form that consumes it had to land together. Full histogram: `brobench/analysis/chunk6_infer_histogram.md`.
  >
  > **Two claims, kept apart** (`types/class_layout.{h,cpp}`, new). A class declaration makes an *identity* claim — "instances of `Vector3` are one compile-time kind" — which is what a `ShapeClassId` already was and which licenses nothing but the cache form. It separately makes a *layout* claim — "`y` is at slot 1" — which is what a constant-offset load consumes, and which is granted only when the whole construction sequence is modellable and **refused with a named reason** otherwise. The layout is a *prediction*; the shape word is the proof. Generated code compares the object's shape against a cell the runtime published only after checking that the key really is an own data property at that slot, so a wrong layout costs a permanently missing guard and never a wrong answer. That is also what let the brief's global verification pass go: `delete` drops the object into dictionary mode, `defineProperty` on a frozen shape likewise, an added property changes the shape — every one of them makes the guard miss, which is exactly what a miss is for.
  >
  > **What the refusals cost, measured one at a time.** The first cut proved 49 of three.js's 218 classes. Five refusals were then read again, and four of them were wrong about their own reason:
  > - `ClassDecl::superName` is what the **parser** resolved, and the module linker renames `superClass` — an expression — while leaving it alone. One line, and it was **157 of 169 refusals**.
  > - A field a **method** installs (`EventDispatcher._listeners`, three.js's single most common shape) lands *after* every constructor field, so it cannot move one. Excluding it from the layout instead of refusing the class recovered 105 classes, `Quaternion` and `Euler` among them.
  > - `Object.defineProperty(this, 'id', {value: n})` is a **shape transition**, not a bail: the runtime routes a new key on a shaped object through the ordinary `setProp(defineOwn=true)`. three.js gives `Object3D`, `BufferGeometry`, `Material` and `Texture` their `id` exactly that way, and those four root nearly every `extends` chain in the library. Accessor descriptors stay refused, which is why `MeshPhysicalMaterial` still declines.
  > - `if (c) this.k = a; else this.k = b` installs `k` on **every** path, at the same position on every path. Reading it as a hole refused `Texture` and its nine subclasses.
  >
  > Final: **200 proven, 18 refused (91.7%)**; property accesses **2.1% → 27.8% native**, of which **3,022 sites carry a proven constant slot**.
  >
  > **The measurement that changed the design.** With every site claimed, `hierarchy` was **3% slower than with the mechanism switched off**, two interleaved passes agreeing. The cause is the one receiver whose static class is not its runtime shape: `this` inside a method of a class somebody extends. three.js never constructs a bare `Object3D`, so every `this.matrixWorld` in `updateMatrixWorld` runs on a `Group` or a `Mesh`, and a cell that pins one of those misses on all the others forever — at the hottest site in the scene. The layout was never wrong (a subclass's fields begin with its base's); the **shape compare** is an identity test and loosening it is rung 2. So `this` receivers of extended classes decline the claim and say so — 952 sites given up, and the regression became a gain on all three scenes.
  >
  > **Same-binary seam A/B** (`BRONZE_NO_STATIC_SHAPES=1`, a compile-time seam, so one bro binary and two `app.dll`s; 300 frames, two interleaved passes, ms/frame): `many_meshes` **37.91 → 37.23** (−1.8%), `instanced` **16.14 → 15.50** (−4.0%), `hierarchy` **5.54 → 5.45** (−1.5%). `bench/run_benchmarks.sh --runs 5` both ways: `three_math` **46.41 → 44.48** (−4.2%, PARITY → WIN), `proto_dispatch_churn` −2.4%, `instanced_mesh_churn` −1.8%, nothing else outside noise.
  >
  > **Honest scope.** Fixed-offset **boxed** slots shipped. Field types are proven and recorded (`ClassLayout::fieldTypes`), and nothing consumes them yet: **no unboxed f64 slots**, so the GC contract is untouched — every slot is still a `Value` and every payload word still scans as one. Three oracle cases (`static_shape_layout`, `static_shape_escape`, `static_shape_refused`) pin the edges the brief named — escape to dynamic code, `delete` then re-add, for-in order, JSON round-trip, a prototype accessor installed after instances exist, a conditionally-assigning constructor, `extends` with `super()` interleaving — byte-identical across infer/no-infer, the chunk's seam, three unrelated seams, GC stress, and heap-verify+poison.

- **Chunk 5 (three.js bill): the root bookkeeping that was mallocing per host call, `===` inlined at the site, the computed read inlined at the site — and one ranked item that was not there**:
  > [!NOTE]
  > **The bill, named first.** Chunk 4's native bill (`brobench/analysis/chunk4_native_bill.md`) ranked seven items by ms/frame. Three were built, one **collapsed on inspection**, one was implemented as an unseamed refactor, and two are verdicts rather than mechanisms. `many_meshes` goes **42.4 → 37.2 ms/frame** on a same-binary seam A/B, and the frame's C-heap share goes **7.96 % → 3.33 %**.
  >
  > **Item 1 — GC root bookkeeping** (`runtime/root_slots.h`, new). The bill named `RootedArgs`, which holds a `std::vector<Value>`, so every host and builtin call mallocs. That is true, and it is the *smaller* half. Surveying the actual per-call cost found `ShadowStackFrame` holding a `std::vector<Value*>` that is **constructed empty and immediately grown 1 → 2 → 4 → 8** on every single host call — several mallocs where `RootedArgs` cost one. Both now use small-buffer storage: `RootSlotList` (`Value**`, 16 inline) and `RootValueBlock` (`Value[]`, 8 inline, sized once and never resized). The rooting contract is preserved exactly, and the reason it can be is that **growth moves the LIST, never the slots** — a root's address is an address in the block or on the C++ stack, and the list only records it. A third cost was found while reading: `RootedArgs` and `RootedBlock` **popped forward**, so every pop but the last missed `pop`'s top-of-stack test and fell into a reverse linear scan — an O(argc²) shape hiding in a destructor. They now pop in reverse and every pop hits the fast test. `ShadowStackFrame`'s ctor, dtor, `push`, `pop` and `current` all became inline; `gc.cpp` fell from 200 lines to 90. Seam: `BRONZE_NO_INLINE_ROOTS=1`, which starts every container at capacity 0 — the old malloc-always shape, not a disabled fast path.
  > - **The widths are audited, not believed.** `BRONZE_IC_LOG=1` grew a *GC Root Block Spills* section that counts every block and list that outgrew its buffer, by reason and by width. Over a 50-frame `many_meshes` run the whole process spills **12 times** (nine blocks at widths 9 and 10, three lists at width 16). Sixteen and eight are the right numbers, and if a future workload changes that, the instrument says so.
  >
  > **Item 3 — `===` answered at the site** (`codegen-llvm/llvm_arith.cpp`, `emitStrictEq`). **The bill under-counted this by two orders of magnitude, and the reason is worth recording**: `bronze_strict_eq` never called `recordHelperCall`, so it had never appeared in a `BRONZE_PROFILE` bill at all and chunk 4 could only estimate it from the sampler's leaf share — 0.9–1.2 ms. One line of instrumentation put the real figure on the table: **302,624,503 calls over 360 frames — 840,000 `===` per frame, 85.1 % of the entire helper bill.** The inline is four arms: both operands ≤ `NUMBER_MAX` → `fcmp oeq` on the bitcast doubles; else raw bits equal → true; else tag ∈ {String, BigInt} → the helper; else false. The FP compare is not an approximation of the spec, it **is** the spec — `NaN !== NaN` with identical bits and `+0 === -0` with different bits both fall out of `oeq` for free, which is exactly why the number arm can be one instruction instead of a sequence of special cases. Seam: `BRONZE_NO_STRICT_EQ_INLINE=1`. **302,624,503 → 3,601,170**, a 98.8 % site answer rate; the residue is the String/BigInt rows the inline path hands back by design.
  >
  > **Item 2 — the computed read answered at the site** (`codegen-llvm/llvm_elem_cache.cpp`, new). Chunk 3 built the `(shape, key)` computed-read table and chunk 4 taught it absence; both still cost a call to consult. `emitElemCacheGet` emits the committed hit path: seam word, non-null table, Object tag, `PLAIN` flags, non-null shape, non-dictionary, key witness (number bits, or a boolean's low bit), `mix64(shape ^ mix64(witness)) & 4095`, entry kind/witness/key checks, shape compare, then the depth word — `0` loads the own slot inline, `ABSENT_FLAG` compares `proto_epoch` and yields `undefined`, anything else goes to the helper. **Placement was the design decision**: the probe replaced the *slow* block rather than becoming a fourth arm of the flags switch, so the existing array and typed-array fast paths pay nothing for it. **String keys are deliberately not inlined** — `ElemCacheEntry::key` is an arena *copy*, so a live key string is never the same object and pointer identity could never hit; confirming content is a length compare plus a `memcmp` loop, which is not a guard. Seam: `BRONZE_NO_ELEM_INLINE=1`, and `BRONZE_NO_ELEM_IC=1` folds it off too, because the table it probes is that cache. **`bronze_elem_get` 32,456,338 → 16,254,918** — exactly the number/boolean half, with the string half untouched by construction rather than by accident.
  > - **A silent bug caught only by a count.** The emitted hash did `mix64(shape ^ mix64(mix64(witness)))` — three rounds where the runtime does two. Nothing was wrong: a probe that always misses is *correct*, every oracle case passed, and the only symptom was a micro's helper count sitting at 400,000 instead of moving. Two rounds took it to **13**. A fast path that is merely never taken looks exactly like a fast path that works.
  >
  > **Item 4 — the attribution collapsed, and nothing was built.** The bill said "the 4-way polymorphic IC is searched by a call, not branches." It is not. `emitIcWayScan` (`codegen-llvm/llvm_prop_ic.cpp:33`) already emits way 0 unconditionally and then an **unrolled inline compare chain for ways 1–3** behind a single load of `poly_ic_enabled`; there is no `bronze_ic_poly_lookup` symbol anywhere in the tree. `InlineCacheSite::find` is the *miss-path* re-scan inside `bronze_prop_get`, reached only from paths that never ran the inline scan in the first place. The 3.00 → 13.67 ns micro cliff that motivated the item is compares, a wide PHI and branch misprediction on a genuinely polymorphic site — the price of polymorphism, not of a call. There is no call to remove.
  >
  > **Item 6 — the TLS block, hoisted** (`runtime/tls_block.h`, new). `bronze_tls_block` moved to namespace scope in `bronze::runtime` and an inline `rtTls()` replaced the `extern "C"` accessor at 30 call sites across `object`, `exception`, `rt_prop`, `elem_ic`, `call_out`, `native_fn_memo` and `embed`. **No seam, because the address is the same address** — this is a refactor, not a mechanism, and a seam on it would guard nothing. It is also what makes `root_slots.h`'s and `elem_ic.cpp`'s seam predicates three instructions instead of a call, so items 1 and 2 partly ride on it.
  >
  > **Item 5 — a verdict, not a mechanism.** `emitMathDirectCall` already emits a direct call to `bronze_math_cos` (chunk 3's `dllimport` fix is what made that guard live), so there is **no call overhead left to shave**. The 12 % of `instanced` sitting in `ucrtbase` is `_remainder_piby2_fma3_bdl` and the polynomial — the argument reduction and the correctly-rounded result themselves. A cheaper polynomial changes the last ulp; three.js derives its matrices from Euler → Quaternion; the oracle is byte-identical output. The brief's own rule — if any oracle output changes, the option is dead — disposes of it, and it should not be re-ranked next chunk without a decision about the ratchet first.
  >
  > **Item 7 — the attribution moved off bronze.** After chunk 4 the elem path's residual misses are ~785 a frame, so bronze is no longer where hot string keys are built. What is building them is **bro's embed layer**: `ev::getProperty` makes a bronze string per property read via `rtMakeString`, and `rt_prop_write.cpp:817` builds a `std::string` per write. That is a bro-side change inside a bronze chunk, and it is left named rather than half-done.
  >
  > **Pins**: three oracle cases, hand-derived and pinned **before the first run**, each matching first time. `root_block_widths` (24 lines: argc 0/1/8/9/64, a builtin that receives arguments and then allocates, a callback that collects mid-iteration, a builtin re-entering JS which re-enters the builtin), `strict_eq_inline` (36 lines: NaN five ways, ±0, strings equal by value and not by identity, symbols, bigints against numbers, boxed primitives), `elem_inline_hit` (23 lines). `tests/oracle/pin_matrix.sh` sweeps them: **252 configurations clean** — 7 cases × 2 inference modes × 18 environments (six seams singly and together, plus heap-verify+poison with and without GC stress). Seven new runtime doctests (`tests/runtime/root_slots_test.cpp`) cover what JS cannot see: that a block is inline until it cannot be, that the seam puts every block on the heap, that an argument block is a **real root across a collection** at argc 1/8/9/64, that a frame which outgrew its buffer is still what the collector walks, that out-of-order and stray pops leave the remaining roots intact, and that `RootedBlock` starts undefined at every width.
  > - **The doctests' own GC trap, caught before running rather than after.** The draft helpers built arrays of tagged objects where each allocation left the previous entries unrooted, and a `tagOf()` that registered its key string — an allocation — *after* receiving an unrooted `Value`. This is the third chunk in which the test scaffolding, not the code under test, was the thing that violated the GC contract.
  > - **A standing divergence found, not introduced**: bronze's `String.prototype.concat` throws "called on a value that is not a string" where 22.1.3.5 says `RequireObjectCoercible` then `ToString`. Per the ratchet the wrong answer was **not** pinned; the case uses `Array.prototype.concat` on an array receiver instead.
  > - **A pre-existing failure fixed, not worked around**: 2 of 29 tests failed at clean `53db7a9`. `native_fn_memo_test.cpp` asserts that two code pointers are two objects, and MSVC's `/OPT:ICF` — on by default in the Release `dev` preset — had folded two identically-bodied probe functions into **one address**, so the test's premise was false and the memo was blamed for it. Making the bodies differ restores the premise rather than weakening the assertion.
  >
  > **Results — three.js under bro** (`brobench`, 300 measured frames, wall ms/frame, one binary, all three seams, two interleaved passes each):
  >
  > | scene | seams off (pass 1 / 2) | seams on (pass 1 / 2) | delta |
  > |---|---|---|---|
  > | `many_meshes` | 42.44 / 42.40 | **37.46 / 37.13** | **−5.1 ms (−12.0 %)** |
  > | `instanced` | 16.96 / 17.06 | **16.26 / 16.19** | −0.79 ms (−4.7 %) |
  > | `hierarchy` | 5.93 / 5.92 | **5.59 / 5.61** | −0.32 ms (−5.5 %) |
  >
  > Per seam on `many_meshes` against a 37.81 all-on control: `BRONZE_NO_INLINE_ROOTS` **−2.39 ms**, `BRONZE_NO_STRICT_EQ_INLINE` **−1.41 ms**, `BRONZE_NO_ELEM_INLINE` **−0.96 ms** — summing to 4.76 against a combined 5.1, which is the expected shape for three independent costs. On `instanced`, `strict_eq` is the **only** one outside noise (−0.82 ms), which is what a scene of `Matrix4.compose` and `Quaternion.setFromEuler` should look like.
  >
  > **Results — the native bill** (`brobench/tools/winsample.cpp`, 660 frames, symbols pinned to the local cache): the **C heap 7.96 % → 3.33 %**, compiled app code 56.8 % → 64.9 %, runtime 28.3 % → 24.9 %, GL 4.30 % → 4.37 % (unmoved, as it has been since chunk 1). Total thread cycles **126,165 → 110,073 Mcycles, −12.8 %** — agreeing with the wall clock to within a tenth of a point, which is the whole reason the cycle table is the instrument on this machine rather than the stopwatch.
  >
  > **Results — bronze suite** (5 runs, same-binary seam A/B, infer mode, **every checksum identical**): every row inside noise or favouring seams-on; `instanced_mesh_churn` 133.51 (off 138.10), `object_graph` 48.53 (49.98), `three_math` 48.36 (47.45), `typed_array_loop` 26.36 (26.61), the rest flat. One row, `proto_dispatch`, printed **99.81 in the seams-off pass against 21.46 on** — a 4.6x "win" that would have been the headline. It is not real: three interleaved A/B passes put it at 21.39/21.71, 21.41/19.90, 19.90/19.97, and each seam alone at 20–22. A single outlier on this machine is worth exactly one re-measurement before it becomes a claim.
  >
  > **Deliberately left out**: (1) **string keys in the inline elem probe** — the entry's key is an arena copy, so the check is a `memcmp` loop and not a guard, and half of `bronze_elem_get` is the honest ceiling here. (2) **A faster `sin`/`cos`** — item 5's verdict above. (3) **Item 4** — there was nothing there. (4) **Item 7's actual fix** — it is bro-side. (5) **A resizing `RootValueBlock`** — argument blocks are sized once at construction by definition, and a resize path would be dead code carrying a live invariant.
  >
  > **Verification**: `ctest --preset dev` **29/29** with `oracle`, `threejs` and `pixi` all run (611 s); `tests/bronze_host/run_checks.sh` **24/24** after rebuilding `bronze.exe`, `bronze_runtime_shared` and `bro-headless` for the four appended TLS words; 252/252 pin-matrix configurations; largest new file 295 lines.

- **Chunk 4 (three.js bill): absence in the computed-read cache, the singleton that was never one, an inline for-of — and the first native-level bill**:
  > [!NOTE]
  > **The bill, named first**, and chunk 3 had already named the three items: `bronze_elem_get`'s residual misses were 100 % absent reads that could never fill, `bronze_function_singleton` was 10.81 M (15.2 % of the whole bill), and the iterator protocol was 12.6 M (17.7 %). All three are closed. The helper bill goes **70.94 M → 52.93 M** on `many_meshes` (360 frames).
  >
  > **Mechanism 1 — absence in the computed-read cache** (`runtime/elem_ic.{h,cpp}`). Chunk 3's `(shape, key)` table refused to cache a proven-absent pair, which left it at a 94.4 % hit rate with **every one of the 1.80 M residual misses an absent read**. It now caches absence under exactly the discipline chunk 1's named-property negative IC established — receiver shape plus `bronze_proto_epoch`, and the identical refusal set (dictionary shapes, a non-plain link anywhere on the chain, index-like keys on exotics, a `CheckMissingMember`-claimed receiver) — because the entry *is* an `InlineCache` and asks `describesAbsent()` of the same code the property path asks. Seam: `BRONZE_NO_ELEM_ABSENT=1`. Misses **1.80 M → 785**, a **99.9976 %** hit rate (`entry_empty` 649, `entry_other_pair_collision` 71, `receiver_kind_not_plain` 60, `entry_same_shape_other_key` 5).
  > - **The objection that had to be answered first was arena growth.** A present entry interns its key because the key string is *already* a live property name; an absent one names a string that may exist nowhere else, and `StringHeader::internToArena` is a **copy, not a hash-cons**, so the naive version would have grown the immortal arena by one string per distinct absent key forever. The fix is a per-thread **deduplicating, budgeted** key table (`elemCacheInternKey`, hash-bucketed, `kElemKeyBudget = 8192`), which both halves now share: past the budget the fill is refused with `key_budget_spent` rather than allocating. A doctest pins that two equal live strings intern to one arena pointer.
  >
  > **Mechanism 2 — what `bronze_function_singleton` turned out to be.** Not a singleton problem at all, and identity was never at risk: the helper *already* interns by code pointer, so the 10.81 M calls were 10.81 M **re-lookups of an answer that could not change** — an `unordered_map<bronze_fn_code, ...>` probe per call. Attributing the top sites showed what was calling it: three.js reading `.get`/`.set`/`.has` off a `WeakMap` or `Map` five thousand times a frame. Those are **member reads no inline cache can hold** (a Map's members come from a kind table, not a shape), so each one walked a C ladder to `rtNativeFunction`, which interned, which probed the map. Since function identity is observable, nothing is merged: the memo is a *shorter route to the same object*. Two per-thread direct-mapped tables (`runtime/native_fn_memo.{h,cpp}`) — one on the code pointer, one on `(kind, keyIndex)` — hold **an index into the runtime's interned-native vector, never a `Value`**, so a collection between fill and hit moves the function object and the entry keeps answering. `rtFunctionSingletonAt(index, expect)` re-checks the code pointer at that index and answers `undefined` when it no longer matches, which makes the table self-healing across module unload and renumber. Seam: `BRONZE_NO_FN_SINGLETON_CACHE=1`. **10,811,607 → 400.**
  > - **A standing divergence found, not introduced**: `builtin_map.cpp`'s `kSetMethods` reuses `mapHas`, and `bronze_function_singleton` interns by code pointer, so `new Map().has === new Set().has` is `true` in bronze and `false` in node. The memo did not cause it and does not widen it. Per the ratchet the wrong answer was **not** pinned as expected output; the oracle case asserts the behaviour instead and the comment records the divergence.
  >
  > **Mechanism 3 — for-of stepped inline** (`codegen-llvm/llvm_iter.{h,cpp}`). `iter.open` already classifies the iterable once and records a `kind` in the record, so the step does not have to re-derive anything — it re-checks. `emitIterStep` byte-copies `stepFast`'s **Array arm** into generated code behind a guard chain (seam word, Object tag, `OBJ_FLAGS_ITERATOR`, `KIND_ARRAY`, `done` is a boolean, target is an array, cursor in range, element block is an object), reads `elems[head + idx + 1]`, turns a hole into `undefined`, stores `current` and advances the cursor; `emitIterValue` loads `current` after the same record-kind guard. Anything else — a mutated length, a user-replaced iterator, the wrong kind — falls through to the helper, unchanged. Seam: `BRONZE_NO_ITER_FAST=1`, and it is read **from generated code**, which is why it lives in the TLS block. **`bronze_iter_value` 3,603,477 → 5. `bronze_iter_step` 5,405,282 → 1,801,810** — one residual terminating call per for-of, by design, because the loop's last step is the one that reports `done`.
  > - **Typed arrays are deliberately not in the fast path.** Their step owes detach and resize `TypeError`s and, for the BigInt views, an allocation; a copy of that is not a copy, and the shape of this chunk is "byte-identical arm or nothing".
  >
  > **Pins**: three oracle cases, hand-derived and pinned **before the first run**, byte-identical across infer / no-infer / GC-stress / each seam alone / all three together / heap-verify+poison / heap-verify+poison+GC-stress. `elem_ic_computed_absent` (18 scenarios), `iter_fast_step` (22 scenarios: the hole rule, the length re-read per element that lets a `push` extend the walk and a `length =` end it early, an element replaced ahead of the cursor, a **shifted** array whose element block starts at a non-zero head, nested walks over one array needing two records, destructuring and spread, an array subclass, an exception mid-walk, and the six kinds the inline path refuses each still answering), `native_member_memo`. **Twelve new runtime doctests** cover what JS cannot see: that an answer came from the memo rather than the walk it replaces, that an absent entry survives a collection and is retired by an own-add *and* by a prototype-add, that the arena key table deduplicates, that a stale index answers nothing, that the member memo is keyed on kind as well as key and refuses a value that is not an interned native.
  >
  > **Results — three.js under bro** (`brobench`, 300 measured frames, wall ms/frame, one binary A/B on the three seams, two passes each):
  >
  > | scene | before (all seams off) | after | delta |
  > |---|---|---|---|
  > | `many_meshes` | 49.39 / 46.18 | **48.59 / 45.41** | −0.78 ms (1.6 %), consistent across both passes |
  > | `instanced` | 16.97 / 16.74 | 16.75 / 16.63 | inside noise, as predicted |
  > | `hierarchy` | 6.29 / 6.22 | 6.22 / 6.33 | inside noise, as predicted |
  >
  > **18.0 M helper calls removed for 0.78 ms.** That ratio is the finding, not a disappointment: it prices the residual ABI helper at ~43 ns and says the helper bill is no longer where the frame is. Which is what Half B went to measure.
  >
  > **Results — bronze suite** (5 runs, same-binary seam A/B, every checksum identical): `three_math` 46.25 (off 47.30), `object_graph` 50.64 (50.35), `typed_array_crunch` 56.90 (55.35), `mesh_churn_2k` 90.41 (91.46), `instanced_mesh_churn` 137.79 (149.30), `fib` 10.32 (10.74), `numeric_loop` 33.94 (37.76), `property_access` 10.38 (11.16), `proto_dispatch` 22.00 (22.92), `proto_dispatch_churn` 60.53 (64.53), `typed_array_loop` 25.41 (27.94). **Every delta favours seams-on or is inside noise; nothing regressed.** None of these compiles shared or reads a Map member in a loop, so the suite's job here was the no-regression check, and it passes.
  >
  > **Half B — the native bill** (`brobench/analysis/chunk4_native_bill.md`). WPR refuses without `SeSystemProfilePrivilege`, so the instrument is a purpose-built unelevated leaf-PC sampler (`brobench/tools/winsample.cpp`) that samples only threads the kernel's own cycle counter says are **running**, on a duty-ratio test rather than an absolute cycle delta — because suspending a parked thread to read its RIP is itself what makes its cycle counter move, and the first four versions of this tool each reported a confident wrong answer for that reason or one like it. Two build changes made the answer legible at all: `--emit-shared` now emits a **PDB** for the module it links (`src/cli/link.cpp`), and the Release `bronze_runtime_shared` now carries `/Zi` plus `/DEBUG /OPT:REF /OPT:ICF` (`cmake/bronze_shared_runtime.cmake`) — before which the runtime resolved only to its nearest *export* and put 4.4 % of the frame in `bronze_tls_block_addr`.
  > - **`many_meshes` is a single-threaded frame**: 99.5 % of the process's cycles on one thread, and **true GPU wait is zero**. The split of 44.8 ms: **compiled app code 53.9 % (24.2 ms), bronze runtime 29.2 % (13.1 ms), the C heap 10.0 % (4.5 ms), everything GL 4.3 % (1.90 ms)**.
  > - **The GL layer is exonerated twice over.** The sampler puts `bro-headless` + `nvoglv64` + `win32u` at 1.90 ms; `BRO_GL_PROFILE` independently measures **1.909 ms**. Chunk 1 measured 1.98 ms of a then-100 ms frame — *the absolute cost has not moved*, and every millisecond recovered since has come out of JS. But it is now 37 % of Chromium's **entire** 5.1 ms frame, so it becomes the wall at ~12 ms/frame, and the fix there is architectural (record and replay off-thread), not micro.
  > - **`instanced` is a different scene entirely**: 75.7 % compiled app code, **12.0 % `ucrtbase` sin/cos**, runtime down to 7.5 %, C heap to 0.9 %. `Matrix4.compose` 14.1 %, `Matrix4.toArray` 12.7 %, `Quaternion.setFromEuler` 7.6 %. The brief asked which of matrix compose / the typed-array write convention / NaN-box packing / bounds checks dominates, and the micro rules three of them out one at a time: a constant-index store costs **2.67 ns into a `Float32Array` and 2.67 ns into a plain `Array`** (not the typed array), a `Float64Array` store costs **11.33 ns against `Float32Array`'s 11.67** (not the conversion, not the box), sixteen consecutive loads plus sixteen consecutive stores cost **60 ns — 1.9 ns per access** (not the bounds check, which hoists), and the identical body written as a real method taking two arguments also costs **60.00 ns** (not the call convention). What is left is a uniform ~5.9x gap against V8 spread over arithmetic, property reads and stores, plus memory: the real `Matrix4.toArray` streams into a 1.28 MB buffer and costs 105 ns where the cached micro costs 60.
  > - **A correction to chunks 1 and 3.** `elem_get`'s long-quoted "11–12 ns" was never the element read — it was the `i & 7` in the probe's own loop. Isolated, **`i & 15` alone costs 10.33 ns** and a computed element read with a counter index costs **4.33 ns**. Any ranking that priced computed element access at 12 ns was pricing the bitwise operator.
  > - **The ranked next chunks**, with ms/frame: (1) **GC root bookkeeping and the C-heap traffic it drives, 3.0–5.0 ms** — `ShadowStackFrame::push`/`pop`/`requireFrameForRoot` are 4.0 % and the heap another 10.0 %, and `RootedArgs` holds a `std::vector<Value>`, so **every host and builtin call mallocs and frees**; fitting the three scenes gives ~800 ns/draw and ~26 ns/object, which is exactly the shape of 10,006 GL calls a frame. (2) computed element access still calls out, **1.5–2.0 ms**. (3) `bronze_strict_eq` calls a helper, **0.9–1.2 ms**. (4) the 4-way IC is searched by a call rather than by branches (micro cliff 3.00 → 13.67 ns), **0.5–1.0 ms**. (5) `Math.sin`/`cos` go to ucrtbase, **0.8–1.4 ms on `instanced`**. (6) `bronze_tls_block_addr` is not hoisted, 0.4 ms. (7) string keys are still being built on hot paths, 0.7 ms.
  >
  > **Deliberately left out**: (1) **typed arrays in the inline iterator path** — detach/resize `TypeError`s and BigInt allocation put them outside "byte-identical arm or nothing". (2) **A generated-code inline for the member memo** — the memo is a runtime-side table and the reads that hit it are exotic-receiver reads that generated code does not inline at all; inlining the probe belongs with rank 2. (3) **The `RootedArgs` fix itself** — it was implemented, measured (`many_meshes` 44.78 → 43.54, `hierarchy` 6.22 → 6.12, `instanced` 16.75 → 16.52) and then **reverted**, because a change to the root-rooting contract needs its own oracle cases and GC-stress runs, and because back-to-back *control* passes on this machine spread 44.78 → 47.84 ms, which puts the delta inside the noise floor. It is rank 1 for the next chunk, and the instrument that will settle it is the sampler's cycles-per-thread table (`many_meshes` control = 221.1 Mcycles/frame), not wall clock. (4) **Inclusive attribution** — the sampler is leaf-PC only, which is why rank 1's caller is inferred from three scenes' draw:object ratios rather than read off a stack.

- **Chunk 3 (three.js bill): the intrinsic guard that was dead in every shared build, and a cache for the computed read**:
  > [!NOTE]
  > **The bill, named first**, and the first job was to read it. `BRONZE_PROFILE=1` on `many_meshes` (360 frames, 121.07 M helper invocations) put `bronze_dynamic_call` at **50.13 M — 41.4 %** of the whole bill, and **28.8 M of those were attributed to nothing at all**: the profiler printed `fn (native/unnamed)` because a native callee carries no recorded name. A bill's largest line item being anonymous is not a measurement, so the profiler learned to name callees before anything was optimised — `profileSetCalleeNamer` plus a by-code-pointer table that `builtin_math.cpp` fills at install time, and, on the bro side, `ObjectBuilder::def`/`accessor` now pass the member name through `embed::makeFunction` (one line covering ~590 `.def(` and ~169 `.accessor(` sites, which also gives every host function a correct `.name` and 10.2.9's `"get "`/`"set "` prefixes).
  >
  > **The answer was not host GL.** The 28.8 M was `Math.sin` 12.6 M, `Math.cos` 10.8 M, `Math.max` 1.8 M, `Math.min` 1.8 M, plus `bronze_array_push` 1.81 M — every one of them a call bronze has an *intrinsic* for, going out through the generic dynamic-call helper instead.
  >
  > **Root cause — a Windows import thunk wearing the runtime's name.** `emitMathDirectCall` recognises an intrinsic by comparing the callee's `FunctionHeader::code` against the ABI symbol's address (`fn->code == abi.bronze_math_cos`). Under `--emit-shared` the module links against `bronze_runtime_shared.dll`, and without `dllimport` the linker resolves each ABI symbol to a **COFF import thunk in the calling module** — a `jmp [__imp_...]` stub whose address is not the runtime's function address. The compare therefore never matched, and **every code-pointer identity guard in the compiler — the six Math intrinsics and `bronze_array_push` — had been silently dead in every shared build for the whole campaign.** The fix is four lines: `declareAbiSymbols` now stamps `DLLImportStorageClass` on every ABI declaration when `sharedRuntime`, so the call goes through the IAT to the real address. ELF and Mach-O need nothing (a GOT entry already holds the definition's address).
  > - **Why no test or benchmark caught it**: the pin is only in *shared* builds. Every oracle case and every `bench/` benchmark compiles **standalone**, links the runtime statically, and calls the definition directly — where the guard always worked. Only bro, brobench and `--emit-shared` were affected, which is precisely where nobody was comparing IL against wall time.
  > - The hypothesis going in was that three.js's `const cos = Math.cos` module-scope aliases defeated intrinsic recognition. **They do not**: micro-repros show an alias intrinsifies fine, because `propGetKey_` flows through SSA. The inversion in the original bill (plain `Math.sin(x)` dynamic, alias intrinsified) was an artifact of the same thunk.
  >
  > **A second dead guard, next door**: `llvm_call.cpp` refused to pad a call whose callee declares more formals than the site passes once the gap exceeded a hard-coded **8** slots. three.js's `setBlending` (arity 10, called with 1) sat just past it and went dynamic 1.8 M times a run. The literal is now a named `kPadSlots = 16`.
  >
  > **Mechanism 1 — a cache for the COMPUTED read** (`runtime/elem_ic.h`, `elem_ic.cpp`). `bronze_elem_get` was 32.5 M entries, 26.8 % of the bill, and **100 % of them a PLAIN receiver with a number, string or boolean key** — the generated code already inlines array and typed-array receivers, so the premise that typed arrays dominate here did not hold and no new generated-code inlining was warranted. What was missing is that a computed read has **no per-site cache word at all**: the helper takes no site pointer, and a property site's entry is keyed on shape *alone*, which is sound only because its key is a compile-time constant. Two evaluations of one `o[k]` can name two properties, so an entry must pin the KEY as well. Rather than widen all ~29k property sites by a word for the minority of reads that are computed, computed reads get a **direct-mapped, per-thread (shape, key) table of 4096 entries** that reuses `InlineCache` verbatim — so `describes`, `describesAbsent` and the accessor/absent flags are asked of the same code the property path asks and cannot drift from it. A number key is answered **without ever being stringified**, which is why the two big buckets converge instead of the numeric one staying dearer. Seam: `BRONZE_NO_ELEM_IC=1`.
  > - **The bucket hash is the whole performance story.** The first version mixed `shape ^ (witness * K)`. A small integer's *double* has ~40 zero low mantissa bits, so `witness * K` also had ~40 zero low bits and the bucket depended almost entirely on the shape — every numeric key on one object collided into one entry. Hit rate 78 %, with 5.4 M `entry_same_shape_other_key`. Splitmix64 on both inputs took misses **7.2 M → 1.80 M (94.4 % hit rate)**.
  > - **GC**: an entry holds a `Shape*` (immortal arena), an **arena-interned** `StringHeader*`, and integers. No `Value`, nothing movable — the same discipline chunk 1's site table keeps, for the same reason: this table is never scanned, and module BSS is not a root.
  >
  > **Mechanism 2 — the runtime call-out** (`runtime/call_out.h`). `Array.prototype.sort`'s comparator went through `bronze_dynamic_call` per comparison, re-answering per element a question that cannot change between two elements of one sort (is this an object, a function rather than a proxy, does its arity fit) — 54,000 times a frame on three.js's render list for one answer. `DirectCallee::bind` asks once. What it must **not** do is cache the answer as a pointer, so the binding is a *fact* and `code`/`env_record` are re-read through the caller's root on every call. Seam: `BRONZE_NO_DIRECT_CALLOUT=1`.
  > - **One deliberate semantic change, spelled out**: the `NewTargetScope(undefined)` push — the expensive half, a rooted slot and a list link per call — moves out of the per-comparison loop and around the whole sort. The old per-call shape meant anything the runtime ran *between* two comparisons (`ToNumber` of a comparator's answer, which can reach a user `valueOf`) saw whatever new.target was ambient outside the sort; under one hoisted scope it sees `undefined`. 13.3.12 says a plain call's new.target IS undefined and every call the sort makes is a plain call, so the hoisted answer is the correct one and the old shape was reporting a constructor's new.target to a function it never constructed. The hoist is therefore **not under the seam** — both settings answer identically, and `array_sort_callout` pins it.
  >
  > **Pins**: two oracle cases, hand-derived and pinned **before** the first run, byte-identical across infer / no-infer / GC-stress / `BRONZE_NO_ELEM_IC` / `BRONZE_NO_DIRECT_CALLOUT` / both / heap-verify+poison / heap-verify+poison+GC-stress — 36 configurations. `elem_ic_computed_read` (16 scenarios): a number key naming a string property, one shape with two keys alternating, one shape many receivers, a key string built at run time, own-shape growth, delete/re-add, a prototype hit plus the epoch that retires it, two links up then a nearer shadow, `setPrototypeOf`, an accessor that must run on every read, a dictionary receiver, a Proxy whose trap count is pinned, a boolean key, the `-0`/`NaN`/fraction/`1e21` key edges with `Object.keys` order, a String wrapper's synthesized properties, and slot-not-value re-read. `array_sort_callout` (13 scenarios): stability, comparator arities the binding treats differently, a bound comparator, a comparator that mutates the array mid-sort, one that throws, non-number and user-code-`valueOf` answers, new.target inside comparator *and* `valueOf` while the sort runs inside a constructor, holes and undefineds, and a 200-element sort exercising every merge width. **14 new runtime doctests** (80 assertions) cover what JS cannot reach: that a right answer came from the *cache* rather than the walk it replaces (a cache that never hits passes all 16 oracle scenarios), that an entry survives repeated collections, that `DirectCallee` refuses a callable Proxy, and that a bound callee may relocate between `bind` and `call`.
  > - Two bugs the pins caught. `BRONZE_GC_STRESS=1` failed the doctests' own helper — a `put(obj, name, value)` that built the key string (an allocation) *after* receiving an unrooted value, so the stale value was stored and the property read back as its own key. And the seam guards initially ran *before* the lazy runtime start-up that reads the `BRONZE_NO_*` environment, so start-up put the env's answer back and the result depended on whether an earlier test in the binary had already woken the runtime.
  > - **A standing divergence found, not introduced**: bronze's `Array.prototype.sort` rejects a **callable Proxy** as comparator, where 23.1.3.30 step 1 asks `IsCallable` and a proxy over a function answers true. Left alone in this chunk (widening step 1 is a semantics change, not a perf one) and deliberately *not* pinned as expected output — recording a wrong answer as an expectation is worse than leaving it unpinned. The doctest covers the binding's own refusal instead.
  >
  > **IC log**: ten `elem_*` refusal reasons (`seam_disabled`, `receiver_not_object`, `receiver_kind_not_plain`, `receiver_no_shape`, `receiver_dictionary`, `key_kind_uncacheable`, `entry_empty`, `entry_epoch_stale`, `entry_other_shape`, `entry_same_shape_other_key`, `entry_accessor`, `proto_walk_refused`), plus per-(shape, key) attribution printed as `{roughness,projectionMatrix,normalM...}.modelViewMatrix` and `{32926,32823,2960}[2929]` — which is what made the bucket-collision bug legible in one run.
  >
  > **Results — the helper bill on `many_meshes`** (360 frames, total dynamic ABI helper invocations):
  >
  > | | total | `bronze_dynamic_call` |
  > |---|---|---|
  > | chunk 2 baseline | 121.07 M | 50.13 M (41.4 %) |
  > | + dllimport & pad cap | 90.46 M | 19.52 M |
  > | + sort call-out | **70.94 M** | **below the top-20 cut (< 10 k)** |
  >
  > `bronze_elem_get` stays 32.46 M *entries* — the cache does not remove the call, it answers it: **1.80 M misses, 94.4 % hit rate**.
  >
  > **Results — three.js under bro** (`brobench`, 300 measured frames, wall ms/frame, one binary A/B on the two seams, two passes each):
  >
  > | scene | before (both seams off) | after | speedup |
  > |---|---|---|---|
  > | `many_meshes` (5,000 meshes, 5,000 draws) | 53.96 / 54.58 | **49.55 / 50.44** | **1.08–1.10x** |
  > | `hierarchy` (5,460 nodes, 256 draws) | 7.26 / 7.61 | **6.97 / 7.05** | **1.04–1.08x** |
  > | `instanced` (20,000 instances, 1 draw) | 17.18 / 17.31 | 17.41 / 17.20 | 1.00x (101 k helper calls in the whole run — correctly untouched) |
  >
  > The seam A/B measures the two *cached* mechanisms only. The dllimport and pad-cap fixes change **generated code** and cannot be seamed; their effect is the helper-count table above, and they are the reason `many_meshes` sits at ~54 ms with the seams off rather than the ~90 ms chunk 1 measured before them. The sort call-out on a dedicated micro: **1.997 / 2.023 → 1.827 / 1.843 ms per round (8.8 %)**.
  >
  > **Results — bronze suite** (5 runs, same-binary seam A/B, all checksums identical): `three_math` 48.68 (off 49.30), `object_graph` 53.17 (54.18), `typed_array_crunch` 58.84 (58.93), `mesh_churn_2k` 98.15 (102.35), `instanced_mesh_churn` 148.22 (149.82), `fib` 11.50 (11.88), `numeric_loop` 36.93 (37.39), `property_access` 11.51 (11.86), `proto_dispatch` 22.82 (22.82), `proto_dispatch_churn` 64.06 (65.08), `typed_array_loop` 27.10 (27.58). **Every delta is ≤ 4 % and every one favours seams-on** — no regression, and none expected, since these compile standalone and none of them reads a computed key or sorts with a comparator in a loop. Absolute numbers run ~8 % above chunk 1's logged figures across the board *including `fib`*, which touches none of this chunk's code; that is a machine-level offset, which is exactly why the seam A/B rather than the absolute column is the regression check. (The churn rows swing wildly when the suite is run on a loaded machine: one pass read `proto_dispatch_churn` at 144 ms and the next at 64 ms, standalone 63.92. Read them same-session A/B or not at all.)
  >
  > **Deliberately left out**: (1) the **generated-site call IC** keyed on callee identity — the brief's own cut-last item, and after the dllimport fix `bronze_dynamic_call` is no longer in the top 20, so there is nothing left at that site to cache. (2) **Absence caching in the computed-read table** — this is now the single largest remaining `elem_get` cost, and it is measured: **1.80 M of the 1.80 M residual misses are `entry_empty` on stable (shape, key) pairs**, i.e. absent reads that can never fill. A micro confirms it exactly (100,000 absent computed reads, 100,000 misses; the present key misses once). Adding it would take the hit rate from 94.4 % to ~99.99 % and is the obvious next item. (3) **`bronze_elem_set`** — 10,965 entries in the whole run, not a target. (4) `bronze_function_singleton` (10.8 M, 15.2 %) and the **iterator protocol** (`iter_step` + `iter_value` + `iter_open` + `for_in_keys` = 12.6 M, 17.7 %), both out of scope by the brief and both now larger shares than anything this chunk touched.
  >
  > **A note on where the frame time now is**: at ~250 k helper calls per frame at ~40 ns, the whole ABI helper bill is ≈ 10 ms of `many_meshes`'s ~46. The remaining ~36 ms is generated code and host GL calls. Further helper-side work has a hard ceiling from here.

- **Chunk 1 (three.js bill): the absent read arms a cache, and a site holds four shapes**:
  > [!NOTE]
  > **The bill, named first**: `brobench/analysis/chunk1_bill.md` measured three.js r160 under bro and found the single largest line item to be that **a property read which finds nothing never arms an inline cache**. three.js's duck-type marker probes (`object.isMesh`, `.isLight`, `.isInstancedMesh`, `.isBufferGeometry`, …) walk own slots plus the whole prototype chain at **~220 ns each, every frame, forever** — worth 27.1 ms/frame of `many_meshes` (28 %) and 8.05 ms/frame of `hierarchy` (45 %), by A/B. The same sites are also **polymorphic**: `Mesh`/`Group`/`Scene`/`Light` all reach them, so a one-entry cache would thrash even if absence were cacheable. The two are one change — either alone leaves the other one missing (measured: a 4-shape absent probe reads 9.5 ms with both on, 132 ms with the negative seam off, 207 ms with the poly seam off, 125–141 ms with both off).
  >
  > **Mechanism 1 — the negative (absent) entry**: a read that walked own slots *and every prototype link to the end of the chain* without finding the key installs `(receiver shape → absent)`, after which generated code answers `undefined` from the entry with no walk at all. The flag is a bit in the entry's depth word (`BRONZE_ABI_IC_DEPTH_ABSENT_FLAG = 0x40000000`) rather than a shape sentinel, so the site is still exactly N entries of shape+slot+depth+epoch and no header word changed. Validity = *receiver shape unchanged* (covers own-property adds) **and** `bronze_proto_epoch` unchanged (covers everything above it). Seam: `BRONZE_NO_NEG_IC=1`.
  > - **The invalidation contract is CHECKED, not assumed.** Every mutation that can make an absent key present bumps the epoch — non-dictionary add when `shape->used_as_prototype`, all dictionary adds/kind-changes unconditionally, `defineAccessor` on a prototype, `setPrototypeOf`/`__proto__` (unconditional, and it dictionaries the object too) — and the generated inline write arm refuses `used_as_prototype` receivers, so an add to a prototype is always the helper. Rather than trust that audit, `ObjectHeader::chainIsCacheable()` walks the chain at install time and refuses unless **every** link is a plain object with a non-dictionary shape that is *marked* `used_as_prototype`. A chain that cannot bump the epoch cannot get an entry.
  > - **Refusals**: dictionary-mode receiver; a non-plain link anywhere (proxy, array, function, typed array, string exotic — the walk stops there and absence was never proven); index-like keys and `length` (a String exotic synthesises those own properties from its string data, not from a shape); and — the subtle one — **any receiver a `*CheckMissingMember` diagnostic claimed**. Those seven refusals `fatal()` for names ECMA-262 defines and bronze has not built, and five of them gate on object *identity* while a negative entry is keyed on *shape*, so a cached miss could have let a same-shaped object skip its diagnostic. Each now **returns whether it claimed the receiver** — the identity test it already ran, returned instead of restated — and a claim blocks the install.
  >
  > **Mechanism 2 — N-way property READ sites**: `InlineCacheSite` is `BRONZE_ABI_IC_WAYS = 4` entries; way 0 is byte-identical to the old single entry, so **write sites, which stay monomorphic, did not change one emitted instruction** (the bill's `bronze_prop_set` misses are 1,034/frame — not a target, not gold-plated). N = 4 from the marker-site evidence: those sites see an Object3D/Mesh pair plus a Scene and a light or two. Eviction is **move-to-front** — install at way 0 shifting the rest down, refill in place when a way already names the shape — which needs no cursor word, so a site is exactly its ways (96 B; ~2.8 MB of module BSS for three.js's ~29k sites). An array-method sentinel is pinned at way 0 and never shifted away, because generated code looks only there for it. Seam: `BRONZE_NO_POLY_IC=1`, which narrows reader *and* writer to one way.
  > - **Emitted shape**: the plain-header check gates the whole scan (an array's or function's shape-offset word is a different field); way 0 compares unconditionally; only a way-0 miss loads `poly_ic_enabled` and enters the way-1..3 compare chain. The depth word then splits three ways on one load, hottest first — `depth == 0` (own slot), `depth == ABSENT_FLAG` (epoch compare, then `undefined`), else accessor/proto. **A monomorphic present read pays nothing**: `property_access` 9.86 ms vs 10.34 ms with the seams off, `proto_dispatch` 20.70 vs 19.76 — both inside noise.
  >
  > **GC**: an IC entry holds `Shape*` (immortal, non-moving arena — never rooted, never relocated), two `uint32`s and a `uint64`. No `Value` is stored, no runtime header gained a word, and the collector's payload scan never reads a module's BSS. The absent flag lives in the low half of the slot word, far from anything the scan reads as a pointer tag.
  >
  > **Pins**: two oracle cases, both byte-identical across infer / no-infer / GC-stress / `BRONZE_NO_NEG_IC` / `BRONZE_NO_POLY_IC` / both / heap-verify+poison, **first run**. `ic_absent_invalidation` — a pinned absent read invalidated by the key appearing on the receiver, on the immediate prototype, two links up, via `Object.defineProperty`, via a prototype *getter*, via `Object.setPrototypeOf`, via `__proto__ =`; plus a null-prototype receiver, a Proxy whose trap count is pinned at 50 (never cached), a dictionary-mode receiver, and `undefined`-as-a-value not being absence. `ic_polymorphic_rotation` — five shapes through a four-way site, four that fit, present-on-some/absent-from-others at one site (the three.js marker shape) with a `Group.prototype.isMesh = true` invalidation mid-loop, own-hit + proto-hit + absent sharing a site, eight shapes rotating forwards and backwards, and fresh shapes minted inside the loop. Four new runtime doctests pin rotation order, refill-in-place, the seam narrowing the writer, the epoch retiring an entry, and `chainIsCacheable`'s three refusals. Full suite green: **29/29**, both milestones (threejs, pixi) included.
  >
  > **IC log**: `poly_ic_ways_free`, `poly_ic_full_rotation`, `negative_ic_epoch_stale`, and four `absent_*` prediction reasons keep the bill's differential parsing exact.
  >
  > **Results — three.js under bro** (`brobench`, 300 measured frames after 60 warmup, wall ms/frame, one binary A/B on the two seams, two passes):
  >
  > | scene | before (both seams off) | after | speedup |
  > |---|---|---|---|
  > | `many_meshes` (5,000 meshes, 5,000 draws) | 94.40 / 90.21 | **54.16 / 49.25** | **1.74–1.83x** |
  > | `hierarchy` (5,460 nodes, 256 draws) | 15.25 / 15.49 | **8.22 / 7.04** | **1.86–2.20x** |
  > | `instanced` (20,000 instances, 1 draw) | 19.68 / 19.89 | 19.64 / 18.94 | 1.00–1.05x (no marker traffic — correctly untouched) |
  >
  > `hierarchy`'s `bronze_prop_get` helper entries over a whole run: **21,615,614 → 634,176**. The plain-object buckets alone go 21,016,302 → 34,864 (**603x**); the non-plain `kind_*` buckets are byte-identical in both runs (599,312), which is the check that nothing else moved. Not one marker key survives in the top-20 miss list.
  >
  > **Results — bronze suite** (idle machine, 5 runs, same-binary seam A/B; no regressions): `three_math` 44.58 (off 45.28) **1.09x WIN**, `object_graph` 49.89 (49.11) **1.39x WIN**, `typed_array_crunch` 55.97 (54.28) 1.01x, `mesh_churn_2k` 91.66 (93.38) 1.01x, `fib` 9.35 (9.11) 4.39x, `numeric_loop` 34.75 (34.66) 1.89x, `property_access` 9.86 (10.34) 3.76x, `proto_dispatch` 20.70 (19.76) 1.75x, `proto_dispatch_churn` 58.56 (58.71) 0.65x, `typed_array_loop` 27.36 (25.14) 1.49x, `instanced_mesh_churn` 135.65 (136.16) 0.70x. Every delta is inside this machine's run-to-run spread — as it must be, since **none of these benchmarks reads an absent property in a loop**. The suite's job here was to prove the new way-scan is free on the monomorphic present read, and it is. (`mesh_churn_2k`, `instanced_mesh_churn` and `proto_dispatch_churn` swing ±40 % at `--quick`'s 3 runs; at 5 runs they reproduce chunk 13's numbers to within 1 %. Read them at 5 runs or not at all.)
  >
  > **Deliberately left out**: property *writes* stay monomorphic (way 0); `in` / `hasOwnProperty` / `Object.hasOwn` do not consult the absent entry — this chunk is plain reads only; and the negative entry does not extend to keyed (computed) reads.

- **Chunk 13: typed_array_crunch ahead of node — proven typed-array element ops and pristine-Math kernels**:
  > [!NOTE]
  > **The bill, named first**: profiling showed helper time ≈ 0; the 2.2x gap was generated-code quality — `--infer-stats` reported 0/90 element operations native and the IL showed the N-body pair loop boxing every `pos[j3]` read, every `acc[i] +=` store, and the `Math.sqrt` call.
  >
  > **Mechanism 1 — proven typed-array element access**: a new lattice element `typedarray:f64|f32` proven at unshadowed `new Float64Array/Float32Array(...)` sites, flowing through the existing join/cell machinery. Lowering emits `elem.get.typed` / `elem.set.typed` (bounds = view length, exactly the helper's rule) only where the consumer coerces — arithmetic sinks, typed-store values, updates, compounds — because the get's out-of-bounds answer is NaN, which is ToNumber(undefined) and nothing else. Codegen inlines the access with the select-before-fptoui poison discipline, invariant view-header loads and typed-array alias scopes. Seam: `BRONZE_NO_TYPED_ELEM=1`.
  > - `definitelyNumericOperand` resolves one-step const chains (`const dx = pos[j] - xi`), always-coercing binaries, and — for `+` alone — both-sides-numeric adds.
  > **Mechanism 2 — proven-number captured cells unbox at read** (`env.get` + `unbox.f64` when inference proved the cell Number).
  > **Mechanism 3 — pristine-Math**: a module-wide taint scan (any bare `Math`, member write/update/delete through it, any `globalThis`, a host manifest naming either) plus per-site shadow checks prove `Math.<own fn>(...)` calls return Number, and license `math.unary` — a bare-instruction lowering for the five bit-exact functions (`sqrt`, `abs`, `floor`, `ceil`, `trunc`; IEEE 754 pins their results, so intrinsic and libm cannot differ in any bit). The N-body `Math.sqrt` drops its global read, property IC, argv spill, guard chain, box and unbox. `sin`/`cos`/`pow`/`min`/`max`/`round` deliberately keep the runtime kernel.
  >
  > **Two bugs the dual-mode ratchet caught while gating**:
  > - The flow rule for `++`/`--` sharpened the binding to `number` unconditionally; 13.4.4 stores ToNumeric, so `let c = 1n; c++` left a BigInt behind a Number proof, and the cell-unbox weaponized it (`bigint_arithmetic` / `bigint_compound_assign` caught it). Fixed: the sharpening now uses `unaryResult`, Number only when the operand can never be a BigInt.
  > - The runtime elem-set helper routed an invalid NUMBER index (NaN, negative, fractional, ≥2^32-1) on a typed array to the named-property hard error, where 10.4.5.5 says silently-discarded store — so a proven-NaN index diverged between modes. Fixed in `rt_prop_write.cpp`; pinned in `typed_elem_semantics`.
  >
  > **Pins**: oracle cases `typed_elem_semantics` (OOB/NaN/-0/fractional/huge indices, silent invalid stores, compounds, updates, f32 narrowing, non-sink `undefined` identity, shadowed constructor), `math_pristine_calls` (IEEE edges incl. `sqrt(-0)`, `ceil(-0.5)` = `-0`; refused shapes: no args, extra args, string arg, optional links), `math_tainted` (write-through taint, replaced method, shadowing) — each byte-equal across infer / no-infer / GC-stress; 8 new types doctests (TypedArray lattice, recognition, shadowing, pristine recording, taint, the BigInt update rule).
  >
  > **Results** (idle machine, median of 5, node v24.2.0 baseline 56.27 ms):
  > - `typed_array_crunch.js`: 145.42 ms → 93 ms (element ops) → 58.74 ms (+cell unbox) → **52.38 ms** (+`math.unary`) — **1.07x, ahead of node**; no-infer 190.93 ms (3.65x inference speedup); `--infer-stats`: 78/90 element ops native (the 12 dynamic are cold init-loop stores of `rng()` chains).
  > - Cross-suite: no regressions — `numeric_loop` 35.63 ms (1.84x WIN), `object_graph` 51.57 ms (1.35x WIN), `three_math` 47.25 ms (1.02x parity), `mesh_churn_2k` 95.35 ms (0.97x parity); `instanced_mesh_churn` (0.67x) and `proto_dispatch_churn` (0.62x) remain the standing losses, identical in both inference modes.
  > - Known pre-existing gap at the time, since CLOSED (chunk 13b below): detached-buffer reads through element paths returned stale bytes and `length` survived a `transfer()`.

- **Chunk 13e: BigInt views get the whole method surface — raw payloads in the loops, values at the boundary**:
  > [!NOTE]
  > 23.2.3's prototype methods over `BigInt64Array`/`BigUint64Array` were the last named refusal on the typed-array surface ("the method bodies convert every element through a double"). Closed without a double ever carrying an element: the method loops move the stored eight bytes (`rawBits64`/`setRawBits64`), and a BigInt VALUE is built only where an element crosses into JS — a callback argument, `at`, an iterator result — through the existing `rtTypedArrayElement` funnel, which already made the ten-Number/two-BigInt split for every other read path in the runtime. Codegen untouched; every changed line is a cold helper body.
  >
  > **The method families**:
  > - *Copying* (`slice`/`toSorted`/`toReversed`/`with`/`copyWithin`): same kind on both sides, so elements move as BYTES — kind-agnostic `memcpy`/`memmove`, which is also the only road a BigInt element has.
  > - *Sort*: elements stage as raw bits (plain C++ memory the comparator's allocations cannot move); 23.2.4.7's default order is the SIGNED reading for BigInt64 and the unsigned for BigUint64 — the same eight bytes sort differently under the two — and a custom comparator receives freshly materialised, rooted BigInts.
  > - *Search* (`indexOf`/`lastIndexOf`/`includes`): the needle is NEVER converted (IsStrictlyEqual/SameValueZero across types is simply false), and each element materialises for a mathematical strict-equality compare — so a needle above 2^64 answers -1 by comparison, not by a wrapping shortcut (2^64 + 2 is congruent to 2 modulo 2^64 but is not 2n; a bits-compare would have "found" it).
  > - *Writes* (`fill`/`with`/`map`'s result/`set` from an array): each converts through 7.1.13 ToBigInt, whose table has no Number row — a Number is a TypeError, never a truncation. `set` from a typed array enforces 23.2.5.1.17's content-type split: mixed types throw; both-BigInt moves as bytes even across the two kinds (the store wraps modulo 2^64 and the bytes already are that answer).
  > - *Callbacks and iterators*: `find*`/`forEach`/`every`/`some`/`reduce*`/`filter`/`map`/`values`/`entries` and `at` swap `fromDouble(elemOf(...))` for the `rtTypedArrayElement` funnel — one line each, generic over both content types. `filter` stages kept elements as raw payloads (a staged vector of BigInt VALUES would be invisible to the collector across the callback).
  >
  > **Adjacent hardening the rewrite forced**: `fill`/`sort` write-backs and the copying family's source reads now clamp to the window as it is AFTER the argument conversions ran (a `valueOf` can shrink it; 23.2.3.8 step 9's re-read is the maintained length field) — previously stale-window writes into dead-but-allocated bytes. `with` converts the value BEFORE judging the index (23.2.3.36 steps 6-7), so a throwing conversion wins over the RangeError.
  >
  > **Pins**: `typed_array_bigint_methods` oracle case — every family above plus `typeof` inside a callback saying `bigint`, INT64_MIN sorting first, the unsigned maximum reading back as -1n through the signed view, the wrapping-trap needle, iterator/for-of over `subarray`, `toString`-is-`join`, and the 13b/13d machinery holding for BigInt views (stranded methods throw; a tracking view recomputes). Both modes ± GC stress ± `BRONZE_NO_TYPED_ELEM` ± `BRONZE_HEAP_VERIFY`+`BRONZE_GC_POISON`, all byte-identical, first run. `bigint64_array`'s refusal note updated; not one expected byte changed anywhere.
  >
  > **Perf** (idle machine, median of 5): `typed_array_crunch` 51.27 ms **WIN 1.10x** checksum unchanged; `typed_array_loop` 24.88 ms **WIN 1.64x** output unchanged — noise-level deltas from 13d's 50.76/23.92, as a cold-helper-only chunk must show.
  >
  > **Still open, named**: iterator resumption fidelity (deliberate, pinned — see 13d); ~~`indexOf`/`includes` on NUMBER views still convert the needle through ToNumber where the spec compares unconverted~~ — closed by the follow-up below.

- **Chunk 13e follow-up: Number-view search needles stop converting**:
  > [!NOTE]
  > 23.2.3.16/.17/.20 never convert the search needle — IsStrictlyEqual/SameValueZero across types is simply false — but bronze's Number-view `indexOf`/`lastIndexOf`/`includes` ran the needle through ToNumber, so a `"2"` needle found 2, `includes(null)` on a zero-filled view answered true, and a BigInt needle threw. Now the needle is compared unconverted on all twelve kinds: a wrong-type needle answers -1/false after the fromIndex conversion has run its side effects (step 4 precedes the loop, and the pin watches the `valueOf` fire). The BigInt arms already did this; the Number arms now match.
  >
  > **Pins**: four new rows in `typed_array_methods` (string/BigInt/undefined needles, the `includes(null)`-on-zero trap, the fromIndex side-effect ordering), all seams byte-identical, first run. Full suite + both milestones green.
  >
  > **Perf**: three cold helper bodies only — `typed_array_crunch` calls none of them and its checksum is unchanged. A same-day re-measure read 54.9–58.6 ms, but with ~20% measured background CPU load (the perf log's loaded-machine rule applies); the standing idle-machine numbers remain 13e's 51.27/24.88. Re-measure when idle before reading anything into a crunch row.

- **Chunk 13d: length-tracking views — the walk already knew how**:
  > [!NOTE]
  > 10.4.5's auto-length views (`new Uint8Array(resizableBuffer)`, no length argument) now TRACK their buffer: length recomputes as `floor((byteLength - byteOffset) / elementSize)` on every resize/grow/transfer, strands only when the buffer drops below the OFFSET (an offset the buffer exactly reaches is an EMPTY window, not an out-of-bounds one), and reopens by recomputation rather than to a remembered count. Cost to codegen: **zero lines**. Chunk 13b's maintained-length design already put the truth in the view's `length` field and re-derived it in `closeOrReopenViews` at every mutation — tracking is one more arm in `refreshLength`, and the inline `index < length` compare, the scoped (never-invariant) length loads, and the iteration fast path are all unchanged and all correct for free.
  >
  > **The sentinel**: a tracking view stores `kAutoLength = 0x7FFFFFFF` in `constructedLength`. NOT `~0u` — the field is the top half of a word the collector's payload scan reads as a Value, and 0xFFFFFFFF's top 16 bits are a valid pointer tag (the scan would "relocate" the sentinel into an address); 0x7FFF stays far below the tag range and `kMaxByteLength` keeps real lengths from colliding. The doctest reads the sentinel back through a collection on purpose.
  >
  > **`subarray` to spec** (23.2.3.30, closing 13c's named divergence): the one prototype method with NO ValidateTypedArray. A detached or stranded source answers — its length clamps to 0 (the maintained field is already there) — and the CONSTRUCTION at the end is the validator (TypeError for a detached buffer, RangeError for an offset the buffer doesn't reach), against the buffer as it is after the begin/end conversions ran. `subarray(begin)` of a tracking source is itself tracking (step 13).
  >
  > **The constructor to spec order** (23.2.5.1): ToIndex(length) runs BEFORE the buffer is measured, so a length `valueOf` that resizes or detaches is judged against the buffer as it is NOW — the same conversion-order fix 13c made for the DataView ctor. Divisibility of the tail binds only a FIXED buffer; a resizable one floors. `SharedArrayBuffer.grow` now runs the view walk (a grow can't strand a fixed view, but a tracking one recomputes from exactly that mutation).
  >
  > **DataView auto-length** (25.3.2.1 step 10): no walk at all — every access is already a helper call, so `trackedByteLength()` measures the buffer when asked. The sentinel is the same value under the same scanned-word constraint.
  >
  > **Pins**: `typed_array_length_tracking` oracle case — grow/shrink/regrow tracking, the boundary empty-vs-OOB distinction, tracking propagation through `subarray`, no-ValidateTypedArray subarray semantics, inline stores and for-of over a mid-loop-grown window, growable-SAB tracking, ctor floor-vs-RangeError and conversion order, auto DataView strand/reopen. Both modes ± GC stress ± `BRONZE_NO_TYPED_ELEM` ± `BRONZE_HEAP_VERIFY`+`BRONZE_GC_POISON`, all byte-identical, first run. `typed_array_oob_throws`'s iterator-strand row now constructs its view with an EXPLICIT length (a lengthless view over that buffer would track and never strand) — intent preserved, not one expected byte changed. The header mechanics doctest replaced the old DOCUMENTED-NOT-ENDORSED one that pinned the fixed-at-construction bug.
  >
  > **Perf** (idle machine, median of 5): `typed_array_crunch` 50.76 ms **WIN 1.11x** checksum unchanged; `typed_array_loop` 23.92 ms **WIN 1.70x** output unchanged. Zero hot-path cost, measured.
  >
  > **Still open, named**: ~~%TypedArray% methods on BigInt views remain named refusals (the double-based method bodies, see rtTypedArrayMember)~~ — closed by Chunk 13e; iterator resumption fidelity — bronze's typed-array iterator resumes if its view's window reopens, where the spec's generator, once done or thrown, is done forever (pinned as bronze's behavior in `typed_array_oob_throws`, deliberate).

- **Chunk 13c: the throwing half of out-of-bounds — methods, iteration, DataView, and the BigInt debt**:
  > [!NOTE]
  > Chunk 13b closed the SOFT surface (element access answers undefined/discard through the maintained length). This chunk closes the three named remainders — everything that per spec THROWS over a closed window — at zero cost to the hot paths, because every new check sits on a path that is already a helper call or a cold branch.
  >
  > **%TypedArray% methods** (23.2.3 ValidateTypedArray): `requireTypedArray` gains the `isOutOfBounds()` arm, so a view a shrinking `resize` stranded — closed but not detached — is a TypeError from every prototype method, not an empty array. Detached was already caught.
  >
  > **Iteration** (23.1.5.1's next asks out-of-bounds BEFORE the length): the for-of fast path (`stepFast`) and the explicit iterator (`taIterNext`) throw for a detached or stranded view — including a transfer MID-LOOP, which now surfaces at the next step instead of ending the loop quietly. The check lives on the `i >= length` branch only (a closed view's length is 0, so every index lands there); the live loop's per-element cost stays the one compare.
  >
  > **DataView** (25.3.1.1-.2 step 6, 25.3.4.2-.3, 25.3.2.1): every accessor asks the new `DataViewHeader::isOutOfBounds()` — detached OR window-overhangs — and throws TypeError, closing a genuine stale-read bug (a detached DataView read the old buffer's bytes). The `byteLength`/`byteOffset` getters throw too (deliberately unlike a typed array's, which answer 0), the constructor refuses a detached buffer at each rung user code could detach on, and the length re-check after a `valueOf` measures the buffer as it is NOW. All helper calls already; no inline path touched.
  >
  > **The BigInt store's debt** (10.4.5.16 converts before it re-checks the index; 7.1.13 has no Number row): the dynamic-store fast paths in `emitElemSet` and the keyed IC arm discarded ANY out-of-bounds store inline — including on BigInt views, where a Number value still owes the ToBigInt that throws. The out-of-bounds edge now lands on a cold kind test (`kind < BRONZE_ABI_TA_KIND_BIGINT64` discards, at/above takes the helper, which converts and throws). Hot in-bounds path byte-identical; this also removed the one behavior that DISAGREED across the `BRONZE_NO_TYPED_ELEM` seam, which is what made the corner unpinnable before.
  >
  > **Pins**: `typed_array_oob_throws` oracle case — stranded-method TypeErrors with regrow recovery, mid-loop transfer, iterator strand/reopen resumption, the full DataView TypeError surface (plus RangeError staying RangeError on a healthy buffer), conversion-before-validity with an observable `valueOf`, the closed-window BigInt Number-store throw, and detached-is-out-of-bounds for zero-length windows. Both modes ± GC stress ± `BRONZE_NO_TYPED_ELEM` ± `BRONZE_HEAP_VERIFY`+`BRONZE_GC_POISON`, all byte-identical.
  >
  > **Perf**: typed_array_crunch re-measured after the codegen change — unchanged (the only emitted-code delta is a cold-edge retarget plus one cold block).
  >
  > **Still open, named**: ~~`subarray` validates where spec doesn't; length-tracking views are still fixed-at-construction~~ — both closed by Chunk 13d; ~~%TypedArray% methods on BigInt views remain named refusals (the double-based method bodies, see rtTypedArrayMember)~~ — closed by Chunk 13e.

- **Chunk 13b: the detach gap closed — the length field IS the witness**:
  > [!NOTE]
  > **The rule**: 10.4.5.9's out-of-bounds witness — a view whose constructed window no longer fits `buffer.byteLength` reads as empty (element reads `undefined`, NaN through the proven coercing ops; writes discarded; `length`/`byteLength` 0; `byteOffset` 0). `transfer` closes every view over the old buffer forever; a shrinking `resize` closes the views it strands; a growing one reopens them — exactly the spec's witness-record behavior.
  >
  > **The design (second attempt — the first cost 8%)**: checking the buffer's current byteLength ON every element access — even folded into an effective length — regressed typed_array_crunch 52→55 ms, because the hot pair loop carries cold guarded IC calls, so a non-invariant buffer load reloads per iteration (disassembly-confirmed). Landed design inverts it: **the view's `length` field itself is maintained**. `transfer`/`resize` call `closeOrReopenViews` (runtime/typed_array.cpp), which collects — the one state in which the heap walks as a gapless run of live, fully-built objects, with the inline-alloc window zeroed — then walks the live space (`Heap::walk_objects`) and sets each affected view's `length` to 0 or back to `constructedLength` (the header word that used to be `reserved`; its ≤2^28 cap keeps the scanned {kind, constructedLength} word non-pointer, same arithmetic as the length word). Every existing `index < length` compare — helper and inline — is thereby also the detach check, at zero per-access cost; the cold mutation pays the walk.
  >
  > **The one codegen change**: view length loads must stop being `!invariant.load` (the walk rewrites them — an invariant CSE across `transfer()` would resurrect the stale-read bug through metadata). They carry a third scoped-alias family instead (`TypedArrayViewLength`, llvm_alias.h): element-data and env stores are declared noalias against it, so the load still hoists out of call-free loops and reloads past any call — the only place a window can move. Cost measured: 51.5–51.9 ms vs 50.6–50.9 same-day pre-fix baseline (~1.5%), still a WIN at 1.08x vs node's 56.27.
  >
  > **Kept in the runtime**: read/write funnels stay plain `index < length`; `rtTypedArraySetElement` re-reads the length AFTER the value conversion (10.4.5.16's order — a `valueOf` that transfers the buffer turns the store into a discard); `byteOffset` getters ask `isOutOfBounds()` (constructed-window form), the one length-family answer the maintained field can't carry.
  >
  > **Pins**: `typed_array_detach` oracle case — transfer/detach across every path shape, the valueOf-mid-store discard, shrink/regrow with byte-survival and zero-fill, offset views; both modes ± GC stress ± `BRONZE_HEAP_VERIFY`+`BRONZE_GC_POISON` ± `BRONZE_NO_TYPED_ELEM` seam, all byte-identical.
  >
  > **Still open, named**: ~~DataView accessors don't consult detach; %TypedArray% prototype methods and iteration see length 0 instead of throwing; inline number-stores into BigInt views discard on a closed window~~ — all three closed by Chunk 13c.

- **Chunk 12: object_graph's bill — Array-Method Loads and Overflow-Slot Transitions**:
  > [!NOTE]
  > **Phase 1 — Sizing the Three Buckets Before Implementation**:
  > 1. **Array-Method Loads (652,622 misses)**:
  >    - Isolated microbenchmark probe: 10M `obj.push` (IC hit) = 1.0 ns/op vs 10M `arr.push` (helper lookup) = 68.5 ns/op (delta: 67.5 ns/call).
  >    - Total helper cost for 652,622 misses (352,204 `.push` + 300,418 `.shift`): **44.05 ms**.
  > 2. **Overflow-Slot Transitions (254,390 misses)**:
  >    - Isolated microbenchmark probe: 1M 4-property objects = 31 ns/obj vs 1M 14-property objects = 360 ns/obj (delta: 32.9 ns per overflow property add).
  >    - Total helper cost for 254,390 overflow transition misses: **8.37 ms**.
  > 3. **Array Shift Element Copying (300,418 calls)**:
  >    - Instrumenting `arrayShift` accumulated **162,102,770 elements moved** (1.24 GB memory copied, max queue length 1,082, avg 539.6 elements/call).
  >    - Measured time spent inside shift copy loops: **~49.0 ms**.
  >    - Total `object_graph` runtime decomposed (~171.6 ms): ~44.1 ms method loads + ~8.4 ms overflow transitions + ~49.0 ms shift copies + ~70.1 ms BFS/DFS/Math/allocations.
  >
  > **Phase 2 Implementation**:
  > - **Phase 2a (Array Method IC Arm)**: Inlined array method resolution in `llvm_prop.cpp` (`emitPropGet`) for array receivers when `ArrayHeader::properties` is not an object. Uses sentinel `cached_shape == 1` (`BRONZE_ABI_IC_SHAPE_ARRAY_METHOD`) and indexes into the GC-rooted `bronze_array_method_tbl` by method ID. Preserves all language semantics (method identity `a.push === a.push`, own-property shadowing `a.push = 5`, deletion restoring builtin `delete a.push`, `a.push = undefined` shadowing, match arrays, arguments). Gated by `BRONZE_NO_ARRAY_METHOD_IC=1`.
  > - **Phase 2b (Inline Overflow Transition Arm)**: Extended `emitPropSet` transition block to support `slot >= 4` when `overflow` is allocated and has sufficient capacity (`overflowCapacity() > slot - 4`). Directly swings receiver shape pointer and stores value into overflow block payload, avoiding helper calls for 7 out of 10 overflow properties (slots 5, 6, 7, 9, 10, 11, 13). Gated by `BRONZE_NO_INLINE_OVERFLOW_SET=1`.
  > - **Attribution & GC Robustness**: Updated `ic_log.cpp` classifiers to attribute honestly (`seam_disabled`, `transition_overflow_alloc_needed`, `transition_overflow_growth_needed`, `array_shadowed_by_side_object`). All singleton method values are registered as permanent root sources in `rt_state.cpp` surviving moving Cheney collections under `BRONZE_GC_STRESS=1`.
  >
  > **Trade Analysis on `kInlineSlots = 4`**:
  > Each `ObjectHeader` with 4 inline slots is 56 bytes. Increasing `kInlineSlots` to 8 (88 bytes) or 16 (152 bytes) would waste 32 to 96 bytes on millions of small 2–4 property objects (`Vector2`, `Vector3`, `Point`, internal scopes, options bags). With 70% of overflow transitions now inlined in generated code with zero helper overhead, `kInlineSlots = 4` remains the optimal trade between footprint and dispatch speed.
  >
  > **Seam A/B on `object_graph.js`** (audit re-measurement on an idle
  > machine; the implementing agent's sweep ran loaded and read high across
  > the board, so its absolute medians were discarded — its deltas held):
  > - Baseline (both arms enabled): **107.08 ms** (infer)
  > - `BRONZE_NO_ARRAY_METHOD_IC=1`: **157.40 ms** (+50.3 ms, vs the ~44 ms
  >   Phase-1 estimate)
  > - `BRONZE_NO_INLINE_OVERFLOW_SET=1`: **112.26 ms** (+5.2 ms, vs ~8.4 ms
  >   estimated)
  > - Both seams disabled: **161.99 ms** (+54.9 ms). Identical checksum in
  >   every variant.
  >
  > **Miss Count Impact (`BRONZE_IC_LOG=1`)**:
  > - `bronze_prop_get` on `object_graph`: **652,745 → 132 misses** (warmup
  >   `ic_uninitialized` plus a few `proto_epoch_stale`; the `kind_array`
  >   reason is gone).
  > - `bronze_prop_set` on `object_graph`: **254,390 → 77,052 misses**, now
  >   attributed 33% `transition_overflow_alloc_needed` + 66%
  >   `transition_overflow_growth_needed` — the allocation points the inline
  >   arm deliberately leaves to the helper (growth can move the receiver).
  >
  > **Audit notes (2026-08-15)**: probes confirmed the sentinel never
  > reaches a set site (lowering allocates separate IC indices for the get
  > and set halves of compound assignment — verified end-to-end with
  > `a.push += ""` over mixed array/plain receivers), shadow flips at IC
  > heat resolve correctly in both directions, and `ensureOverflow` zeroes
  > every fresh word it exposes to the GC payload scan (the chunk-9 residue
  > bug class does not apply). The remaining `object_graph` gap is now
  > dominated by `arrayShift`'s O(n) element copying — measured this chunk
  > at ~162 M elements / ~1.24 GB moved (~49 ms). A head-offset array
  > representation is the named candidate fix, deliberately left to its own
  > chunk because it touches the `ARRAY_*` ABI offsets and every inline
  > element path.
  - `object_graph.js`: **107.08ms** (infer) vs 114.98ms (no-infer) — (checksum=-32601148, **down from 171.62ms: 38% faster, 1.54x behind node's 69.38ms**)
  - `three_math.js`: **37.53ms** (infer) vs 35.82ms (no-infer) — (checksum=405000)
  - `typed_array_crunch.js`: **114.66ms** (infer) vs 123.10ms (no-infer) — (checksum=78849652)
  - `mesh_churn_2k.js`: **131.68ms** (infer) vs 161.41ms (no-infer) — (checksum=-2112298)
  - `instanced_mesh_churn.js`: **132.08ms** (infer) vs 129.58ms (no-infer) — (checksum=1260786)
  - `fib.js`: **7.30ms** (infer) vs 13.94ms (no-infer)
  - `numeric_loop.js`: **34.13ms** (infer) vs 51.50ms (no-infer)
  - `property_access.js`: **9.52ms** (infer) vs 9.91ms (no-infer)
  - `proto_dispatch.js`: **20.55ms** (infer) vs 24.70ms (no-infer)
  - `proto_dispatch_churn.js`: **55.28ms** (infer) vs 58.87ms (no-infer)
  - `typed_array_loop.js`: **33.05ms** (infer) vs 35.02ms (no-infer)

- **Chunk 11: IC Miss Attribution and Shape-Preserving Property Definition**:
  > [!NOTE]
  > **Phase 1 Attribution Diagnosis (`BRONZE_IC_LOG=1`)**:
  > Built an env-gated IC miss attribution subsystem (`src/runtime/ic_log.h`, `src/runtime/ic_log.cpp`)
  > classifying every `bronze_prop_get`, `bronze_prop_set`, and `bronze_dynamic_call` entry by reason,
  > key/callee, and site. Attribution ran across `object_graph.js`, `mesh_churn_2k.js`, and `instanced_mesh_churn.js`
  > in both `infer` and `--no-infer` modes (producing 100% identical byte-for-byte miss classifications):
  > 1. **Three.js Object3D Dictionary Degradation (The Top Cause)**:
  >    On `instanced_mesh_churn.js`, `bronze_prop_get` recorded **2,370,609 misses** (89.0% `receiver_in_dict_mode`: 2,110,973 misses on `.matrix`, `.instanceColor`, `.position`, `.scale`, `.instanceMatrix`, `.setMatrixAt`, `.quaternion`, `.updateMatrix`, `.rotation`, `.setColorAt`) and `bronze_prop_set` recorded **183,035 misses** (82.4% `receiver_in_dict_mode`: 150,817 on `.matrixWorldNeedsUpdate`).
  >    On `mesh_churn_2k.js`, `bronze_prop_get` recorded **1,548,058 misses** (78.6% `receiver_in_dict_mode`: 1,216,846 misses on `.rotation`, `.parent`, `.matrixWorld`, etc.) and `bronze_prop_set` recorded **480,153 misses** (36.8% `receiver_in_dict_mode`: 176,864 misses).
  >    *Root Cause*: Three.js `Object3D`'s constructor invokes `Object.defineProperty(this, 'id', ...)` and `Object.defineProperties(this, { position, rotation, quaternion, scale, ... })`. `rtObjectDefineProperty` previously converted the object unconditionally to dictionary mode (`ObjectHeader::toDictionary()`), preventing all inline caches from caching or hitting on any `Object3D` property.
  > 2. **Dynamic Call Under-Arity Padding**:
  >    On `instanced_mesh_churn.js`, all **300,138** residual `bronze_dynamic_call` misses were attributed 100.0% to `under_arity_padding` (`setRGB` arity 4, argc 3: 150,038 misses; `set` arity 4, argc 3: 150,000 misses), where calls pass 3 arguments to 4-parameter functions expecting `undefined` padding.
  > 3. **Array Prototype Method Gets**:
  >    On `object_graph.js`, `bronze_prop_get` recorded **652,622 misses** (100.0% `kind_array` on `.push` [352,204] and `.shift` [300,418]), while `bronze_prop_set` recorded **254,390 misses** (99.8% `transition_overflow_slot` on slots >= 4).
  >
  > **Phase 2 Fix — Shape-Preserving Property Definition (`builtin_object_descriptor.cpp`)**:
  > Updated `rtObjectDefineProperty` to extend the receiver's Shape transition tree (`shape->addProperty` / `setProp` with `defineOwn=true` or `defineAccessor`) for new properties and compatible attribute updates rather than degrading plain objects to dictionary mode.
  > All `Object3D` instances now traverse a shared, monomorphic Shape transition tree, enabling LLVM-generated monomorphic ICs to hit directly in generated code.
  > Provided an env seam `BRONZE_NO_SHAPE_DEFINE=1` to A/B test.
  >
  > **Impact** (miss counts; runtimes below are the audit's re-measured sweep):
  > - `instanced_mesh_churn`: `bronze_prop_get` misses **2.37M → 260k** (89% drop); `bronze_prop_set` misses **183k → 33k** (82% drop). Total property misses cut by **2.26 Million**.
  > - `mesh_churn_2k`: `bronze_prop_get` misses **1.55M → 360k** (77% drop).
  >
  > Post-chunk audit: the shape-preserving path as first landed broke two
  > 10.1.6.3 rules the dictionary path used to honor. (1) An accessor
  > defined via `defineProperty` was recorded configurable:true — the
  > literal-accessor default — so `delete` removed what the spec says must
  > survive; fixed by threading the descriptor's `configurable` through
  > `ObjectHeader::defineAccessor` (literals keep their true default).
  > (2) `{writable:false}` on a non-configurable writable property — one of
  > the two changes the spec still permits — returned without applying;
  > fixed by demoting that one object to dictionary mode (a shared Shape
  > cannot express the change) and clearing the entry's writable bit.
  > Oracle case `define_property_shape_semantics` pins both rules, the
  > descriptor attribute defaults, and the hot Object3D-style constructor
  > pattern under the GC-stress re-run. The chunk's own sweep (and its
  > "baseline before") ran on a loaded machine — every non-target bench
  > read ~2x high — so the table below is the audit's clean re-run; the
  > mechanism's win is real and LARGER against true baselines. Two
  > pre-existing dictionary-path gaps are now named for future work, not
  > introduced here: redefinition with a partial descriptor clobbers
  > absent fields to their defaults instead of keeping existing values,
  > and any redefinition on a non-configurable dictionary entry throws
  > blanket TypeErrors including for spec-legal no-ops.
  - `three_math.js`: **36.54ms** (infer) vs 36.99ms (no-infer) — (checksum=405000)
  - `object_graph.js`: **171.62ms** (infer) vs 166.46ms (no-infer) — (checksum=-32601148; its bill is kind_array push/shift + transition_overflow_slot, untouched by this chunk)
  - `typed_array_crunch.js`: **117.30ms** (infer) vs 125.40ms (no-infer) — (checksum=78849652)
  - `mesh_churn_2k.js`: **129.01ms** (infer) vs 141.68ms (no-infer) — (checksum=-2112298, **down from 215.83ms; now 1.39x behind node's 92.70**)
  - `instanced_mesh_churn.js`: **130.29ms** (infer) vs 128.86ms (no-infer) — (checksum=1260786, **down from ~278-289ms; now 1.37x behind node's 95.28**)
  - `fib.js`: **8.91ms** (infer) vs 14.84ms (no-infer)
  - `numeric_loop.js`: **36.17ms** (infer) vs 54.03ms (no-infer)
  - `property_access.js`: **12.87ms** (infer) vs 11.48ms (no-infer)
  - `proto_dispatch.js`: **22.21ms** (infer) vs 25.73ms (no-infer)
  - `proto_dispatch_churn.js`: **60.22ms** (infer) vs 62.21ms (no-infer)
  - `typed_array_loop.js`: **34.71ms** (infer) vs 36.07ms (no-infer)

- **Chunk 10: Profile-driven bill knockdown — inlined dynamic calls and prototype-overflow IC**:
  > [!NOTE]
  > Profiled the three unprofiled benchmarks (`object_graph.js`, `mesh_churn_2k.js`, `instanced_mesh_churn.js`)
  > at HEAD before implementing. Profiling evidence:
  > - `BRONZE_GC_LOG=1` confirmed GC collections are 0 on all three target benchmarks (0 collections, 0.000 ms in `collect()`).
  > - `BRONZE_PROFILE=1` identified two massive bills across all three workloads:
  >   1. `bronze_dynamic_call`: **3,571,537 calls** across the three targets (2,151,629 on `instanced_mesh_churn`, 706,597 on `mesh_churn_2k`, 713,311 on `object_graph`).
  >   2. `bronze_prop_get`: **5,783,016 calls** across the three targets (3,275,715 on `instanced_mesh_churn`, 1,854,556 on `mesh_churn_2k`, 652,745 on `object_graph`).
  >
  > Two fast-path mechanisms implemented following the house pattern:
  > 1. **Inlined Dynamic Call (`llvm_call.cpp`, new)**:
  >    In generated code, `Op::DynamicCall` guards on:
  >    - Callee is Object-tagged (`(callee >> 48) == TAG_OBJECT`)
  >    - Callee flags == `HeapKind::Function` (`BRONZE_ABI_OBJ_FLAGS_FUNCTION`)
  >    - `fn->arity <= argc` (FunctionHeader arity check)
  >    - Feature enabled (`bronze_inline_call_enabled != 0`, toggled via `BRONZE_NO_INLINE_CALL=1`)
  >    When all guards pass, generated LLVM IR loads `fn->env_record` and `fn->code` and invokes
  >    `code(env, thisVal, argc, argv)` directly via indirect function call.
  >    On any miss (non-callable, under-arity requiring undefined padding, or seam disabled),
  >    branches to `bronze_dynamic_call`.
  > 2. **Prototype IC Overflow Slot Support (`llvm_prop.cpp`)**:
  >    Extended the depth > 0 prototype walk in `emitPropGet` to support slots >= 4 (overflow slots).
  >    When the target slot index on the prototype holder is >= 4, generated code loads the holder's
  >    `overflow` block (guarding on Object tag) and loads `payload[slot - 4]` directly, mirroring depth-0
  >    overflow reads.
  >
  > Helper invocation reductions:
  > - `object_graph`: `bronze_dynamic_call` 713,311 → **0** (total helper invocations: 2.73M → 2.01M).
  > - `mesh_churn_2k`: `bronze_dynamic_call` 706,597 → **2,002**; `bronze_prop_get` 1.85M → **1.55M** (total helper invocations: 3.42M → 2.41M, over 1.01M calls eliminated).
  > - `instanced_mesh_churn`: `bronze_dynamic_call` 2.15M → **300,138**; `bronze_prop_get` 3.28M → **2.37M** (total helper invocations: 5.66M → 2.90M, **2.76M calls eliminated**).
  >
  > New oracle test `dynamic_call_inline_stress` verifies exact arity, over-arity, under-arity padding fallback,
  > prototype overflow methods, this-binding, TypeError exceptions on non-callables, and heavy allocations
  > during calls under `BRONZE_GC_STRESS=1`. Full test suite 19/19 passing.
  > A/B seam numbers (`BRONZE_NO_INLINE_CALL=1`):
  > - `object_graph`: 167.31 ms vs 172.12 ms (no-inline-call)
  > - `mesh_churn_2k`: 226.28 ms vs 232.64 ms (no-inline-call)
  > - `instanced_mesh_churn`: 272.16 ms vs 285.82 ms (no-inline-call)
  >
  > Post-chunk audit: the inline call path as first landed skipped the
  > helper's `NewTargetScope(undefined)` push with no gate, so `new.target`
  > read inside a plain callee DURING a construction leaked the enclosing
  > constructor instead of undefined — a real miscompile, reproduced in both
  > modes and absent under `BRONZE_NO_INLINE_CALL=1`. Fixed the way the
  > inline `new` path already handles the identical skip: one `new.target`
  > anywhere keeps the whole module's dynamic calls on the helper
  > (`moduleHasNewTarget` in llvm_ops.cpp); neither three.js r160 nor pixi
  > v8.19.0 mentions it, so the fast path is unaffected where it matters.
  > New oracle case `new_target_plain_call_mask` pins the mask. The audit
  > also re-measured two outliers in the sweep below on the fixed build:
  > `property_access` no-infer measured 10.79 ms (the 19.77 was machine
  > noise) and `typed_array_crunch` no-infer 124.57 ms (the 164.63
  > likewise) — both consistent with chunk 9; the lines below carry the
  > re-measured numbers.
  - `three_math.js`: **35.26ms** (infer) vs 34.06ms (no-infer) — (checksum=405000, down from 38.67ms)
  - `object_graph.js`: **176.05ms** (infer) vs 178.88ms (no-infer) — (checksum=-32601148)
  - `typed_array_crunch.js`: **111.23ms** (infer) vs 124.57ms (no-infer, audit re-measure) — (checksum=78849652, down from 115.52ms)
  - `mesh_churn_2k.js`: **222.75ms** (infer) vs 206.54ms (no-infer) — (checksum=-2112298)
  - `instanced_mesh_churn.js`: **278.22ms** (infer) vs 272.22ms (no-infer) — (checksum=1260786, down from 288.74ms / 279.12ms)
  - `fib.js`: **8.04ms** (infer) vs 13.91ms (no-infer)
  - `numeric_loop.js`: **35.30ms** (infer) vs 52.17ms (no-infer)
  - `property_access.js`: **10.36ms** (infer) vs 10.79ms (no-infer, audit re-measure)
  - `proto_dispatch.js`: **21.80ms** (infer) vs 25.15ms (no-infer)
  - `proto_dispatch_churn.js`: **64.36ms** (infer) vs 59.67ms (no-infer)
  - `typed_array_loop.js`: **34.63ms** (infer) vs 34.95ms (no-infer)

- **Chunk 9: Inline allocation for `new` — the constructor fast path in generated code**:
  > [!NOTE]
  > `BRONZE_PROFILE=1` re-verified chunk 8's remaining churn bill at HEAD before
  > building: 3,000,001 `bronze_construct` calls were the ENTIRE
  > `proto_dispatch_churn` residual. One mechanism, the house pattern: guard,
  > inline the helper's committed ordinary path exactly, fall back to the
  > helper on every miss (`llvm_construct.cpp`, new).
  > The guard admits a constructor carrying `FunctionHeader::construct_vetted`
  > — a byte only `bronze_construct`'s ordinary path sets, i.e. after the
  > bound-function and primitive-wrapper probes (both answer by the
  > creation-fixed code pointer, so the byte is monotone-sound) — with
  > `arity <= argc` (the exact condition `FunctionHeader::call` passes argv
  > through unpadded), a non-null `instance_shape`, and headroom in the
  > inline-allocation window. The window (`bronze_alloc_cursor`/`_limit`, ABI
  > data symbols) is from-space the helper carves 8 KB at a time; generated
  > code bump-allocates the 56-byte plain instance from it and NEVER collects
  > — no fit means the helper, which can. Every collection zeroes the window.
  > The fresh instance is rooted in a dedicated frame slot across the
  > constructor call and re-read after, since the constructor's own
  > allocations may move it. Under BRONZE_GC_STRESS the refill is exactly ONE
  > object, so stress alternates inline/helper and shakes the inline rooting;
  > `BRONZE_NO_INLINE_ALLOC=1` keeps the window empty (one-binary A/B: 96 ms
  > vs 172 ms wall-clock medians on churn). The inline path skips the
  > helper's NewTargetScope push, which is sound only because
  > `bronze_get_new_target` is that scope's sole observer — so one
  > `new.target` anywhere in a module keeps every construct site of that
  > module on the helper (neither three.js r160 nor pixi v8.19.0 contains
  > one). Helper invocations on churn: 3.0M → 20.4k (one refill per 146
  > inline allocations). New oracle case `construct_inline_alloc` pins the
  > replace-on-object-return rule, primitive-return rule, the under-arity
  > fallback, and — shaped for the suite's GC-stress re-run — construction
  > whose ctor allocates mid-flight with live references held across every
  > allocation; byte-equal in both modes, stressed and not.
  >
  > Landing this chunk surfaced a LATENT collector bug, older than the
  > chunk: the GC payload scan reads the word of `FunctionHeader` holding
  > `is_generator` — two bools and six bytes of never-written padding — as a
  > Value, and recycled-semispace residue in that padding can parse as a
  > heap pointer. The chunk's one-byte vet write shifted the residue and
  > turned pixi's GC-stress milestone from green into an environment-chain
  > corruption; bisected by disabling the feature piecewise until the only
  > live delta was that byte, then fixed by zeroing the padding explicitly
  > at create() (the `reserved` convention namespace.h and typed_array.h
  > already follow). Details in the fix commit.
  > Node-baseline standing: `proto_dispatch_churn` 3.3x → **1.55x** behind
  > (59.16 vs 38.14). No-regression check across all benches vs chunk 8: all
  > within noise (instanced_mesh_churn 288.74 vs 277.86 is the documented
  > bimodal machine noise — its no-infer twin measured 279.12 vs chunk 8's
  > 298.07 in the same sweep, and a re-run gave 290.90/287.42).
  - `three_math.js`: **38.67ms** (infer) vs 38.33ms (no-infer) — (checksum=405000)
  - `object_graph.js`: **169.23ms** (infer) vs 170.04ms (no-infer) — (checksum=-32601148, down from 175.67ms)
  - `typed_array_crunch.js`: **115.52ms** (infer) vs 127.38ms (no-infer) — (checksum=78849652)
  - `mesh_churn_2k.js`: **215.83ms** (infer) vs 225.08ms (no-infer) — (checksum=-2112298, down from 253.13ms)
  - `instanced_mesh_churn.js`: **288.74ms** (infer) vs 279.12ms (no-infer) — (checksum=1260786; noise band, see note)
  - `fib.js`: **7.95ms** (infer) vs 15.06ms (no-infer)
  - `numeric_loop.js`: **35.82ms** (infer) vs 52.29ms (no-infer)
  - `property_access.js`: **10.97ms** (infer) vs 10.62ms (no-infer)
  - `proto_dispatch.js`: **22.18ms** (infer) vs 24.99ms (no-infer)
  - `proto_dispatch_churn.js`: **59.16ms** (infer) vs 62.25ms (no-infer) — **down from 124.98ms / 129.64ms**
  - `typed_array_loop.js`: **37.13ms** (infer) vs 36.20ms (no-infer)

- **Chunk 8: The dispatch-churn loop's caches, inlined — and Math dispatched direct**:
  > [!NOTE]
  > `BRONZE_PROFILE=1` re-verified Chunk 7's named bill at HEAD before building
  > (churn: 3M each of `bronze_function_singleton` / depth>0 `bronze_prop_get` /
  > shape-transition `bronze_prop_set` / `bronze_construct`; crunch: 1.37M
  > `bronze_global_get` of `Math` + 1.53M `bronze_dynamic_call`, 1.37M of them
  > to Math natives). Four mechanisms, each keeping its helper's committed
  > semantics and falling back to it on every guard miss:
  > 1. **Depth > 0 proto-hit reads inline** (`llvm_prop.cpp`): the IC entry's
  >    epoch word is checked against the prototype-mutation epoch (now the ABI
  >    data symbol `bronze_proto_epoch`) and the shape→root→prototype chain is
  >    walked in generated code, mirroring `cachedProtoHolder` exactly —
  >    dictionary on the path, non-plain link, stale epoch, overflow slot all
  >    fall back. The Shape fields the walk reads moved ahead of the
  >    transitions vector and are ABI-pinned (`shape.h` static_asserts).
  > 2. **Shape-transition write IC** (`object.cpp` + inline arm in
  >    `llvm_prop.cpp`): a set-site entry whose cached shape is `parent ==
  >    receiver shape` and whose own node carries the site's key (slot
  >    uniqueness makes `slot_index == cached_slot` that check) takes the
  >    recorded transition — no own-miss walk, no inherited-setter walk, no
  >    transition scan. Guarded by the same epoch discipline the read entries
  >    use; `used_as_prototype` receivers keep the helper (it owes the bump).
  > 3. **Rooted-table caches read inline** (`llvm_cache.cpp`, new):
  >    `bronze_function_singleton` swapped its linear scan (every native
  >    builtin ever interned was on it) for a by-code-pointer map, plus a
  >    published `{code, value}` slot table generated code checks first —
  >    self-validating by code-pointer compare, so embed-style slot collisions
  >    refill rather than break identity. `global.get` reads the published
  >    `g_globalCache` table the same way. Both tables' cells are GC roots
  >    forwarded in place, which is what makes an inline read of a moving-heap
  >    cache sound.
  > 4. **Math direct dispatch** (`llvm_math.cpp`, new): a dynamic call whose
  >    callee was read as `sqrt`/`sin`/`cos`/`abs`/`min`/`max` guards the
  >    callee's `FunctionHeader::code` against the intrinsic's exported symbol
  >    (`bronze_math_*` in the ABI registry) and number-checks the arguments;
  >    sqrt/abs inline as llvm.sqrt/llvm.fabs (IEEE-exact), sin/cos/min/max
  >    call the exact scalar kernels the helper path itself runs. No
  >    fast-math anywhere; `Math.sqrt = f` misses the pointer compare.
  > Helper invocations: churn 12.0M → 3.0M (only `bronze_construct` remains
  > — the inline-allocation fast path was deliberately NOT built this chunk;
  > any inline allocation can move every live value and is gated on the
  > GC-stress milestones), crunch 2.9M → 0.2M. Three new oracle cases pin the
  > guards' invalidation behavior (`ic_proto_depth_epoch`,
  > `ic_transition_setter_shadow`, `math_direct_dispatch`), each byte-equal
  > in both modes and under BRONZE_GC_STRESS=1. Full suite 19/19 including
  > both GC-stress milestone runs. Node-baseline standing:
  > `proto_dispatch_churn` 7.6x → **3.3x** behind (124.98 vs 38.14),
  > `typed_array_crunch` 2.3x → **2.1x** (117.78 vs 56.27; its residual is
  > now the 155k closure calls and raw loop code, not Math). `mesh_churn_2k`
  > shows ±20 ms run-to-run variance on this machine (241–263 ms across
  > re-runs, no-infer 232–258); treat its delta as noise.
  - `three_math.js`: **39.17ms** (infer) vs 38.12ms (no-infer) — **0.97x** (checksum=405000, node 48.40ms → bronze wins)
  - `object_graph.js`: **175.67ms** (infer) vs 178.82ms (no-infer) — **1.02x** (checksum=-32601148, down from 195.69ms)
  - `typed_array_crunch.js`: **117.78ms** (infer) vs 130.55ms (no-infer) — **1.11x** (checksum=78849652, down from 127.54ms)
  - `mesh_churn_2k.js`: **253.13ms** (infer) vs 232.36ms (no-infer) — **0.92x** (checksum=-2112298; see variance note)
  - `instanced_mesh_churn.js`: **277.86ms** (infer) vs 298.07ms (no-infer) — **1.07x** (checksum=1260786, down from 286.52ms)
  - `fib.js`: **8.55ms** (infer) vs 15.89ms (no-infer) — **1.86x**
  - `numeric_loop.js`: **36.54ms** (infer) vs 58.00ms (no-infer) — **1.59x**
  - `property_access.js`: **11.15ms** (infer) vs 11.97ms (no-infer) — **1.07x**
  - `proto_dispatch.js`: **22.29ms** (infer) vs 26.06ms (no-infer) — **1.17x** (down from 28.29ms)
  - `proto_dispatch_churn.js`: **124.98ms** (infer) vs 129.64ms (no-infer) — **1.04x** (down from 290.56ms)
  - `typed_array_loop.js`: **34.47ms** (infer) vs 36.16ms (no-infer) — **1.05x** (node 40.75ms → bronze wins)

- **Chunk 7: GC/Allocation Pass — measurement first, then helper inlining**:
  > [!NOTE]
  > The pass opened with measurement (`BRONZE_GC_LOG=1`, a new env-gated stderr
  > report in `runtime/heap.cpp`: collections, bytes allocated, bytes copied,
  > wall time inside `collect()`), and the measurement killed the working
  > hypothesis: **the semispace GC is a non-factor on every losing benchmark**.
  > `proto_dispatch_churn.js` spends 0.017 ms of a 306 ms run in `collect()`
  > (5 collections, 0.02 MB copied of 160 MB allocated — the live set is
  > tiny); `object_graph.js`, `mesh_churn_2k.js` and `typed_array_crunch.js`
  > run to exit with **zero** collections. A generational nursery was
  > therefore not built. `BRONZE_PROFILE=1` named the real bill — helper
  > calls per loop iteration — and the pass inlined the four biggest:
  > 1. Environment slot access (`llvm_env.cpp`, new): `env.get` /
  >    `env.get.tdz` / `env.set` become inline loads/stores — parent-chain
  >    walk unrolled at the compile-time depth, brand and slot-range guards
  >    kept, every failure edge still reaching the helper's fatal or
  >    ReferenceError. `typed_array_crunch.js` alone made 44.5M of these
  >    calls (49% of its helper total).
  > 2. Dynamic-index element access (`llvm_elem.cpp`, new): `v[i]` on an
  >    Array (hole→undefined) and on Float32/Float64 typed arrays, load and
  >    store, mirroring the `bronze_elem_get`/`_set` fast paths exactly —
  >    35.1M calls in crunch, 12.3M in `typed_array_loop.js`.
  > 3. `bronze_dynamic_add`: the number/number case becomes an inline fadd
  >    with the canonicalizing re-box (19M calls across the losing set).
  > 4. `bronze_rel_lt/gt/le/ge`: the number/number case becomes one ordered
  >    fcmp (NaN→false on all four, which is 13.10's undefined→false).
  > New ABI layout constants (`EnvHeader`, `TypedArrayHeader`,
  > `ArrayBufferHeader`, header size word) pinned by static_asserts in the
  > runtime headers. `typed_array_crunch.js` helper invocations: 90.8M →
  > 2.9M. Node-baseline standing: `typed_array_loop` and `three_math` flip
  > to **wins** (0.85x / 0.88x of node), crunch improves 5.8x→2.3x,
  > `object_graph` 3.4x→2.8x, `proto_dispatch_churn` 8.5x→7.6x. Churn's
  > remaining bill is named for the next pass: `bronze_construct` +
  > `bronze_function_singleton` + shape-transition `prop_set` + depth>0
  > `prop_get` at 3M each. (results.jsonl is rewritten by each runner
  > invocation; this run used `--pure-only`.)
  - `three_math.js`: **42.47ms** (infer) vs 42.67ms (no-infer) — **1.00x** (checksum=405000, node 48.40ms → bronze wins)
  - `object_graph.js`: **195.69ms** (infer) vs 200.82ms (no-infer) — **1.03x** (checksum=-32601148)
  - `typed_array_crunch.js`: **127.54ms** (infer) vs 136.07ms (no-infer) — **1.07x** (checksum=78849652, down from 329.04ms)
  - `mesh_churn_2k.js`: **241.06ms** (infer) vs 234.83ms (no-infer) — **0.97x** (checksum=-2112298)
  - `instanced_mesh_churn.js`: **286.52ms** (infer) vs 280.50ms (no-infer) — **0.98x** (checksum=1260786)
  - `fib.js`: **8.96ms** (infer) vs 14.22ms (no-infer) — **1.59x**
  - `numeric_loop.js`: **34.82ms** (infer) vs 53.19ms (no-infer) — **1.53x**
  - `property_access.js`: **10.58ms** (infer) vs 11.22ms (no-infer) — **1.06x**
  - `proto_dispatch.js`: **28.29ms** (infer) vs 33.66ms (no-infer) — **1.19x**
  - `proto_dispatch_churn.js`: **290.56ms** (infer) vs 289.38ms (no-infer) — **1.00x** (down from 324.99ms)
  - `typed_array_loop.js`: **34.69ms** (infer) vs 36.86ms (no-infer) — **1.06x** (node 40.75ms → bronze wins, down from 90.18ms)

- **Chunk 6: Real-Library Shape Optimization & IC Performance**:
  > [!NOTE]
  > Shape specialization, dynamic inline cache (IC) activation, array element/length fast paths, and overflow slot inline caching:
  > 1. `SuperCall` root frame accounting (`llvm_func.cpp`): Added `il::Op::SuperCall` to `maxArgc` computation in `planRootFrame()`, resolving constructor stack corruption on deep Three.js derived classes (e.g. `DepthTexture`).
  > 2. `llvm_prop.cpp` IC fast path activation: Enabled inline shape guards for all property get/set sites without requiring static monomorphism proofs.
  > 3. Direct Array element & length fast paths: Generated inline loads/stores for constant index keys (`0`..`15`) and `.length` on `HeapKind::Array` without helper calls.
  > 4. Out-of-line overflow slot fast path: Emitted inline load/store paths for object properties at slot >= 4 in the `overflow` payload block.
  > 5. Runtime helper call drop: Total dynamic ABI helper calls reduced by **51.2%** across the 4 real-library benchmarks (from 76.4M to 37.3M invocations; `three_math.js` -71.5%, `mesh_churn_2k.js` -50.6%, `object_graph.js` -52.4%, `instanced_mesh_churn.js` -44.7%).
  - `three_math.js`: **57.34ms** (infer) vs 59.81ms (no-infer) — **1.04x** (checksum=405000, 38.2% faster than Chunk 1 92.80ms)
  - `object_graph.js`: **236.81ms** (infer) vs 236.38ms (no-infer) — **1.00x** (checksum=-32601148)
  - `typed_array_crunch.js`: **329.04ms** (infer) vs 379.29ms (no-infer) — **1.15x** (checksum=78849652)
  - `mesh_churn_2k.js`: **266.38ms** (infer) vs 264.46ms (no-infer) — **0.99x** (checksum=-2112298, 20.2% faster than Chunk B 334.01ms)
  - `instanced_mesh_churn.js`: **313.10ms** (infer) vs 312.66ms (no-infer) — **1.00x** (checksum=1260786)
  - `fib.js`: **22.47ms** (infer) vs 41.24ms (no-infer) — **1.84x**
  - `numeric_loop.js`: **48.95ms** (infer) vs 70.54ms (no-infer) — **1.44x**
  - `property_access.js`: **30.59ms** (infer) vs 36.80ms (no-infer) — **1.20x**
  - `proto_dispatch.js`: **48.25ms** (infer) vs 72.73ms (no-infer) — **1.51x**
  - `proto_dispatch_churn.js`: **324.99ms** (infer) vs 347.04ms (no-infer) — **1.07x**
  - `typed_array_loop.js`: **90.18ms** (infer) vs 107.71ms (no-infer) — **1.19x**

- **Chunk 1: Fix instanced_mesh_churn Regression & SSA Boxing Inlining**:
  > [!NOTE]
  > Optimization pass targeting `instanced_mesh_churn.js` 0.91x regression and dynamic boxing/conversion overhead:
  > 1. `box.f64` / `box.i32` / `box.bool` (LLVM Codegen): Inlined SSA boxing directly into LLVM IR via bitcast and NaN-canonicalization select, eliminating ABI helper calls on loop-carried scalar values.
  > 2. `unbox.f64` (LLVM Codegen): Fast-path numeric unboxing checking `tag <= BRONZE_ABI_NUMBER_MAX_BITS` and bitcasting directly from i64 to double.
  > 3. `bronze_prop_get` / `bronze_prop_set` (Runtime): Precomputed `KeyInfo` structure classifying integer-like string keys ("0".."15") and "length" at key registration time, enabling $O(1)$ direct element accesses on `ArrayHeader` and `TypedArrayHeader` without GC rooting or string parsing.
  > 4. `bronze_dynamic_add` / `isLessThan` (Runtime): Direct numeric fast paths bypassing GC frame creation, `rtToPrimitive`, and exception checks when operands are numbers.
  > 5. `bronze_elem_get` / `bronze_elem_set` (Runtime): Fast non-negative integer check without `std::floor` overhead.
  - `instanced_mesh_churn.js`: **315.50ms** (infer) vs 353.70ms (no-infer) — **1.12x inference speedup** (checksum=1260786, fixed 0.91x regression)
  - `three_math.js`: **92.80ms** (infer) vs 94.34ms (no-infer) — **39.6% faster** (infer down from 153.56ms)
  - `object_graph.js`: **235.30ms** (infer) vs 247.78ms (no-infer) — **18.5% faster** (infer down from 288.77ms)
  - `typed_array_crunch.js`: **320.18ms** (infer) vs 384.47ms (no-infer) — **41.8% faster** (infer down from 550.04ms)
  - `property_access.js`: **26.86ms** (infer) vs 37.61ms (no-infer) — **48.9% faster** (infer down from 52.54ms)
  - `proto_dispatch.js`: **46.18ms** (infer) vs 70.21ms (no-infer) — **48.5% faster** (infer down from 89.69ms)
  - `proto_dispatch_churn.js`: **313.58ms** (infer) vs 360.08ms (no-infer) — **23.1% faster** (infer down from 407.88ms)
  - `typed_array_loop.js`: **98.94ms** (infer) vs 104.02ms (no-infer) — **45.4% faster** (infer down from 181.07ms)

- **Chunk B: First Optimization Pass (Top Dynamic Helper Hot Paths)**:
  > [!NOTE]
  > Optimization pass targeting top profile helpers from Chunk A Fallback Report:
  > 1. `bronze_prop_set` (Helper #2): Inlined monomorphic property write cache fast path in codegen (`emitPropSet`) and propagated `icMonomorphic` in lowering; inlined slot writes.
  > 2. `bronze_elem_get` / `bronze_elem_set` (Helper #3): Fast numeric index dispatch for ArrayHeader and TypedArrayHeader element access avoiding symbol/string conversion overhead and bounds-checked direct stores.
  > 3. `bronze_dynamic_call` (Helper #4): Fast-path direct function code invocation and 32-slot stack buffer for arity adaptation, eliminating `std::vector` heap allocations on under-arity method calls.
  > 4. `bronze_prop_get` (Helper #1): Direct Array/TypedArray length and buffer fast paths plus inlined `getSlot`/`setSlot` accessors.
  - `three_math.js`: **153.56ms** (infer) vs 172.97ms (no-infer) — **1.13x inference speedup**
  - `object_graph.js`: **288.77ms** (infer) vs 280.72ms (no-infer) — **3.0% faster** (infer down from 297.78ms)
  - `typed_array_crunch.js`: **550.04ms** (infer) vs 757.09ms (no-infer) — **14.5% faster** (infer down from 643.02ms / 655.47ms, no-infer down from 852.13ms)
  - `mesh_churn_2k.js`: **334.01ms** (infer) vs 356.65ms (no-infer) — **6.1% faster** (infer down from 355.82ms, no-infer down from 379.55ms)
  - `fib.js`: **22.71ms** (infer) vs 76.19ms (no-infer) — **3.35x inference speedup**
  - `numeric_loop.js`: **49.90ms** (infer) vs 156.26ms (no-infer) — **3.13x inference speedup**
  - `property_access.js`: **52.54ms** (infer) vs 72.11ms (no-infer) — **1.37x inference speedup**
  - `proto_dispatch.js`: **89.69ms** (infer) vs 170.62ms (no-infer) — **1.90x inference speedup**
  - `proto_dispatch_churn.js`: **407.88ms** (infer) vs 489.58ms (no-infer) — **1.20x inference speedup**
  - `typed_array_loop.js`: **181.07ms** (infer) vs 199.91ms (no-infer) — **21.5% faster** (infer down from 230.65ms, no-infer down from 253.91ms)
  - `render_scenegraph_host`: **450.61ms** (compiled bro-bronze-host, 30 frames — down from 460.05ms)
  - `render_wild_orbit_host`: **508.83ms** (compiled bro-bronze-host-wild, 30 frames)
  - `render_interpreted_bro`: **437.34ms** (interpreted QuickJS, 30 frames)

- **Chunk C: Build-Type Truth & Release Baseline**:
  > [!NOTE]
  > Compile-time and execution figures in entries above were measured with a Debug compiler build and are superseded by these verified Release figures (`build_type: "Release"`).
  - `three_math.js`: **148.08ms** (infer) vs 162.11ms (no-infer) — **1.09x inference speedup**
  - `object_graph.js`: **297.78ms** (infer) vs 303.28ms (no-infer) — **1.02x inference speedup**
  - `typed_array_crunch.js`: **643.02ms** (infer) vs 857.41ms (no-infer) — **1.33x inference speedup**
  - `mesh_churn_2k.js`: **355.82ms** (infer) vs 379.55ms (no-infer) — **1.07x inference speedup**
  - `fib.js`: **23.34ms** (infer) vs 79.05ms (no-infer) — **3.39x inference speedup**
  - `numeric_loop.js`: **48.73ms** (infer) vs 155.80ms (no-infer) — **3.20x inference speedup**
  - `property_access.js`: **71.39ms** (infer) vs 88.03ms (no-infer) — **1.23x inference speedup**
  - `proto_dispatch.js`: **87.10ms** (infer) vs 166.44ms (no-infer) — **1.91x inference speedup**
  - `proto_dispatch_churn.js`: **393.17ms** (infer) vs 506.19ms (no-infer) — **1.29x inference speedup**
  - `typed_array_loop.js`: **230.65ms** (infer) vs 253.91ms (no-infer) — **1.10x inference speedup**
  - `render_scenegraph_host`: **460.05ms** (compiled bro-bronze-host, 30 frames)
  - `render_wild_orbit_host`: **497.92ms** (compiled bro-bronze-host-wild, 30 frames)
  - `render_interpreted_bro`: **450.93ms** (interpreted QuickJS, 30 frames)

- **Chunk 1: Benchmark Harness Baseline**:
  - `fib.js`: **22.77ms** (infer) vs 476.48ms (no-infer) — **20.93x inference speedup**
  - `numeric_loop.js`: **48.74ms** (infer) vs 751.07ms (no-infer) — **15.41x inference speedup**
  - `typed_array_crunch.js`: **5086.68ms** (infer) vs 8053.74ms (no-infer) — **1.58x inference speedup**
  - `proto_dispatch.js`: **778.77ms** (infer) vs 1539.76ms (no-infer) — **1.98x inference speedup**
  - `property_access.js`: **424.18ms** (infer) vs 512.26ms (no-infer) — **1.21x inference speedup**
  - `three_math.js`: **1439.06ms** (infer) vs 1460.23ms (no-infer) — **1.01x**
  - `mesh_churn_2k.js`: **2636.39ms** (infer) vs 2726.82ms (no-infer) — **1.03x**
  - `object_graph.js`: **2082.22ms** (infer) vs 2061.81ms (no-infer) — **0.99x**
  - `typed_array_loop.js`: **1901.88ms** (infer) vs 2157.08ms (no-infer) — **1.13x**
  - `render_scenegraph_host`: **446.66ms** (compiled bro-bronze-host, 30 frames)
  - `render_wild_orbit_host`: **535.25ms** (compiled bro-bronze-host-wild, 30 frames)
  - `render_interpreted_bro`: **449.83ms** (interpreted QuickJS, 30 frames)

