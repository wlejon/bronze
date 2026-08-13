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
node bench/fib.js
node bench/numeric_loop.js
node bench/property_access.js
node bench/proto_dispatch.js
node bench/proto_dispatch_churn.js
node bench/typed_array_loop.js
```

## The Benchmark Log

Measurements recorded on this machine (median of 5 runs, warmup discarded):

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
