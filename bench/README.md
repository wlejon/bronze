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

