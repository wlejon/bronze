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

