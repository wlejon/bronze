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

Measurements recorded on this machine (median of 5 runs, warmup discarded):

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
  > **Still open, named**: `subarray` validates where spec doesn't (throws on a detached source; spec builds the view regardless) — conservative, pre-existing, and harmless for real code; %TypedArray% methods on BigInt views remain named refusals (the double-based method bodies, see rtTypedArrayMember); length-tracking views (auto-length over resizable buffers) are still fixed-at-construction.

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

