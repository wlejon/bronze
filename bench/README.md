# Benchmarks

Deterministic benchmark harness and runner for Bronze and the Bro runtime integration.
Measures execution performance across compilation modes (inferred native layouts vs uniform dynamic convention) and compares compiled host apps against interpreted QuickJS.

> [!IMPORTANT]
> **Automated runner hard rule**: `node` is NEVER invoked by the automated runner or test suite (see `CLAUDE.md`).
> Benchmarks measure Bronze against its own history and execution modes. Manual instructions for running outside Bronze (e.g. in Node.js) are documented below for manual reference only.

## Running Benchmarks

Run via the bash runner:

```bash
bench/run_benchmarks.sh          # Full suite (5 runs per case, warmup discarded)
bench/run_benchmarks.sh --pure-only   # Pure-compute scenes only (no GL/DOM)
bench/run_benchmarks.sh --render-only # Bro WebGL/scenegraph render scenes only
bench/run_benchmarks.sh --filter math # Filter benchmarks by name/description
bench/run_benchmarks.sh --json        # Machine-readable JSON-lines only
```

Or via the alias:
```bash
./bench/bench.sh
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

Measurements recorded on this machine (median of 5 runs, warmup discarded):

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

