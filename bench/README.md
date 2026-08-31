# Benchmarks

Deterministic benchmark harness and runner for Bronze and the Bro runtime integration.
Measures execution performance across compilation modes (inferred native layouts vs uniform dynamic convention) and compares compiled host apps against interpreted QuickJS.

> [!IMPORTANT]
> **This suite measures COMPUTE, not processes.** Every fixture times its own
> compute region with `performance.now()` through [`harness.js`](harness.js) and
> prints `[bench] <region> ms=<f>` on stderr; every tool here reads that line and
> none of them consults a process wall clock. Process startup is not a fact about
> the compiler — an empty bronze program costs ~5.9 ms on this box and an empty
> node program ~31 — and a stopwatch around the process reports mostly that
> difference. The checksum stays on stdout, so `cmp` on two captured stdouts is
> still the miscompile check and the timing never enters it.
>
> The one exception is the Bro render scenes: they are host applications from
> another tree that cannot be taught to self-report, so their rows are measured
> as whole-process wall and are labelled `wall` in the JSON.

> [!IMPORTANT]
> **Automated runner hard rule**: `node` is NEVER invoked by the automated runner or test suite (see `CLAUDE.md`).
> The runner compares Bronze's execution modes against each other and against the
> pinned medians in `node_baselines.json`. A LIVE node column exists only in
> `bench/tools/interleave.py`, which is a manual instrument — that is where a
> node number gets taken in the same session as the bronze ones it is compared
> against, instead of being remembered from another.

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
| `repr_slot_kernel.js` | 400k iterations over pinned number fields, inline and out-of-line, with never-written shape-mates beside them | Per-slot double representation: the store arms' Number test, the collector over a mixed heap. Needs `--pins bench/pins/repr-slot-kernel.pins` to make any slot a double one |
| `repr_flow_kernel.js` | 400k iterations chaining load-arithmetic-store across two objects whose every field is pinned, plus a computed-NaN score in the checksum | The raw store and the elided GC root between links, and the NaN frontier a raw store could alias a tag at. Needs `--pins bench/pins/repr-flow-kernel.pins` |
| `proto_dispatch.js` | Depth-3 inherited property read (stable epoch) | Prototype chain IC caching at depth > 0 |
| `proto_dispatch_churn.js` | Depth-3 inherited read with interleaved `new Pt(i)` | Ordinary object property adds vs prototype cache invalidation |
| `typed_array_loop.js` | Float32Array element access vs plain Array, timed as two separate regions | TypedArray buffer access vs JS array indexing |

### 1b. Kernels

A kernel isolates one operation and reports `ns_per_iter` beside its
millisecond total, because a total says nothing without the count beside it.

| Kernel | Description |
|---|---|
| `mat4_kernel.js` | `Matrix4.multiplyMatrices` ns/call through the real vendored three.js class, 20M calls |
| `env_slot_kernel.js` | A factory closure whose hot state is captured variables, three.js's `WebGLState` shape — env-record slot access. Needs `--pins bench/pins/env-slot-kernel.pins` for the slots written from parameters |
| `env_slot_kernel_registers.js` | The same arithmetic and the same checksum with nothing captured, so the state lands in SSA registers. The control that separates "a slot access costs something" from "the state being in memory costs something" |
| `nullish_pin_kernel.js` | `number-or-nullish` fields through a hot `step`, including an alternating-instance loop so neither arm is the only one the predictor learns. Needs `--pins bench/pins/nullish-kernel.pins` |
| `call_chain_kernel.js` | A chained typed call edge against the same arithmetic inlined, timed as two regions, because the RATIO between them is the measurement. Needs `--pins bench/pins/call-chain-kernel.pins` |

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

Compute region only, taken under the protocol above. Pinned in
`node_baselines.json`; re-measured 2026-08-27 on an idle box, 51 interleaved
rounds, medians.

| Benchmark | Node compute (ms) | ns/iter | Verified Output / Checksum |
|---|---:|---:|---|
| `fib.js` | 6.08 | — | `832040` |
| `numeric_loop.js` | 28.96 | 2.896 | `60644102826883.61` |
| `property_access.js` | 1.06 | 1.063 | `3000000` |
| `proto_dispatch.js` | 1.64 | 0.546 | `3000000` |
| `proto_dispatch_churn.js` | 3.13 | 1.043 | `3000000` |
| `typed_array_loop.js` typed / plain | 2.67 / 2.39 | 1.305 / 1.166 | `523826421.8828082` / `523828354.8980187` |
| `typed_array_crunch.js` | 19.06 | — | `typed_array_crunch checksum=78849652` |
| `object_graph.js` | 32.28 | — | `object_graph checksum=-32601148` |
| `three_math.js` | 8.80 | — | `three_math checksum=405000` |
| `mesh_churn_2k.js` | 32.33 | — | `mesh_churn_2k checksum=-2112298` |
| `instanced_mesh_churn.js` | 33.61 | — | `instanced_mesh_churn checksum=1260786` |

> [!NOTE]
> **V8 is a speculative JIT, and two of these rows show it.** `proto_dispatch`
> at 0.55 ns/iter is under two cycles for what the source spells as a depth-3
> prototype walk, and `property_access` at 1.06 is not far behind: on a long
> monomorphic loop V8 hoists what it proves invariant, and part of what those
> rows measure is that proof rather than the cost of a read. Treat them as an
> upper bound on the gap, not as a dispatch-cost comparison. The rows built out
> of real library code — `three_math`, `mesh_churn_2k`, `instanced_mesh_churn`,
> `object_graph` — have no such escape hatch and are the honest ones.

## Where bronze stands

The three library-scale rows, taken under layout control with
`bench/tools/layout_sweep.py` — nine link orders per fixture (the order bronze
ships plus eight seeds), three independently-created copies of each, medians per
seed. The bronze column is the SHIPPED order's median, not a pooled one, because
that order is now chosen rather than arbitrary (below); the spread beside it is
how far the same objects read under the eight permutations. **Compute region
only — no process startup in any cell.** The checksum was identical on every run
of every column, which is the acceptance condition: a difference in a checksum
is a miscompile, and a comparison that does not check them is not a measurement.
The node column is the pinned baseline from `node_baselines.json`; nothing here
invokes node.

| fixture | objects | bronze default | cross-seed spread | node | vs node |
|---|---:|---:|---:|---:|---:|
| `three_math` | 3 | **11.41** | 1.00 (8.5%) | 8.80 | 0.77× |
| `mesh_churn_2k` | 10 | **35.50** | 1.10 (3.1%) | 32.33 | 0.91× |
| `instanced_mesh_churn` | 16 | **38.55** | 2.95 (7.6%) | 33.61 | 0.87× |

**The spread column is the point of the table.** A large fixture is emitted as
several partition objects, and the order they are handed to the linker decides
where every function lands in the image. Nothing else changes — same objects,
same symbols, same program — and across twenty-five orders of the same
`instanced_mesh_churn` objects the region read 37.1 ms under the best and
40.9 ms under the worst, a 9.9% band that reproduces: the same seed comes out in
the same place in the ranking on every repeat of the sweep, so this is
placement, not noise, and no number of extra rounds averages it away.
`mesh_churn_2k`, with ten objects instead of sixteen, sits in a 3% band;
`three_math` has only three objects and therefore only six orders to draw from
at all.

**What the band is made of, and what bronze now does about it.** Re-linking the
same sixteen `instanced_mesh_churn` objects in orders chosen by hand rather than
by seed says where the band comes from. Swapping two objects that hold no code
the fixture executes moves the region by 0.1–0.4 ms, which is the 0.17 ms two
byte-identical links of one order read apart — so nothing global (alignment,
image size, how far the code sits from the runtime) is doing this. Moving the
partitions that DO hold the hot frames toward the front, most-used first, moves
it by 3.8 ms, and monotonically: the hot loop's partition alone is worth 1.8 ms,
the top two 2.7, the top four 3.4, all eight 3.8. The band is one thing —
whether the partitions that call each other end up near each other — and the
order bronze shipped before was the packer's, which sorts bins by SIZE. That is
the arrangement that reliably scatters a working set, because a loop and the
small leaf bodies it calls have nothing in common except the call edge, and it
measured at the 96th percentile of the twenty-five-order distribution: nearly
the worst order available.

So the order is now decided instead of inherited (`src/codegen-llvm/llvm_partition.h`,
`partitionUsesLinkOrderPolicy`): start at the partition that owns the entry
point, then repeatedly hand over whichever partition is most tightly tied by
symbol references to the one just handed over. It is static, it reads nothing
but the module, and the same input gives the same order and the same bytes.
`BRONZE_NO_LINK_POLICY=1` restores the old order and is the A/B seam — the
objects are byte-identical under it, only the sequence changes. On
`instanced_mesh_churn` the shipped order reads 38.18 and 38.91 ms across two
sweeps against the old order's 40.95 and 40.56, and `mesh_churn_2k` and
`three_math` move by less than their own noise.

That is most of the fixture's distance from the fast edge, not all of it: a
profile-fed order — the hot partitions first, in the order the sampler ranks
them — reaches 36.7–37.1 over four sweeps, better than any of the twenty-five
seeded orders. That gap is what a static rank cannot see. Nothing in a
partition's static shape says it is hot: the packer balances the bins to within
3% of each other on instruction count, and this fixture's hot frames land in
eight of the sixteen, spread across the whole size spectrum. Two are not even
reachable in the module's direct-call graph — the calls into `Euler.set` and
`Quaternion.setFromEuler` go through an inline cache, and they carry 11.6% of
the samples. Weighing the affinity edges by the referenced body's size, or by
the smaller of the two bodies, was tried and measured a millisecond WORSE than
counting them; the bins are balanced, so any size-scaled reading is dominated by
how much code a bin holds, which is the same for all of them.

Choosing the order narrows nothing: the band a seed can still reach is as wide
as it was, and it is wider than most of the deltas this suite is asked to
adjudicate, which is why the protocol changed. An A/B that links each arm once compares one
arbitrary draw against another and reports the difference as a result: editing
four NEVER-EXECUTED lines of the vendored three.js bundle — three parameter
defaults on `Color` methods this fixture never calls and one comparison inside
`WebGLRenderer` — measured **+5.0%** on `instanced_mesh_churn` that way, and
**+0.9%, inside the bar,** across nine orders. So:

> **An arm-vs-arm delta is CLAIMABLE only if it exceeds the wider of the two
> arms' cross-seed spreads.** `layout_sweep.py` prints that rule and applies it.

A per-seed point costs a LINK, not a compile: `bronze build --keep-objs <dir>`
leaves the partition objects behind and `bronze link <dir> --link-seed <n>`
relinks them under a deterministic permutation. On the `instanced_mesh_churn`
graph that is ~0.3 s of link against ~90 s of object emission, which is what
makes nine layouts per arm affordable.

### The `--pins` opt-in, under the same layout control

`--pins` is not a tuning flag and is not on by default: a manifest is a set of
DECLARATIONS about a program's fields, parameters and returns that inference is
told to believe, and a pinned array's elements compile to raw f64 loads and
stores with no guard at all. It is enforced rather than assumed — a store,
argument or return that violates an entry throws a catchable `TypeError` naming
the manifest line (`src/types/pins.h`, `tests/cli/pin_barrier_test.cpp`), so a
manifest that is wrong about a path diagnoses instead of corrupting. Nobody has
to write one by hand: `bronze build --census <out.pins>` instruments the
program, a representative run joins what it observed, and the entries that were
monomorphic become the file (`docs/pin-census.md`).

Each fixture below was censused from itself, the `@observed` entries dropped
(what a default build accepts), and the resulting manifest swept against the
same objects' unpinned twin — nine link orders each, three copies, the decision
rule above.

| fixture | census entries (safe / all) | bronze default | pinned | pinned spread | node | pinned vs node | verdict |
|---|---:|---:|---:|---:|---:|---:|---|
| `three_math` | 34 / 37 | 11.30 | **7.56** | 0.16 (2.1%) | 8.80 | **1.16×** | CLAIMABLE, −33% |
| `mesh_churn_2k` | 80 / 100 | 33.49 | 33.84 | 0.75 (2.2%) | 32.33 | 0.96× | not claimable (+1.0%, bar 0.80) |
| `instanced_mesh_churn` | 72 / 155 | 38.36 | 38.74 | 4.84 (12.6%) | 33.61 | 0.87× | not claimable (+1.0% / −1.6%, bar 4.84) |

**`three_math` under pins is the one row in this suite where bronze beats node
on a library-scale region.** 7.56 ms against node's pinned 8.80, and the margin
is not a layout draw: it is 1.24 ms against a 0.39 ms cross-seed spread on the
wider arm, and the pinned arm's WORST of nine orders (7.64) still beats node by
more than a millisecond. What buys it is one entry. Unpinned, `Matrix4.invert`
and `Matrix4.multiplyMatrices` carry 18% of the profile's samples between them;
under `Matrix4.elements: numeric-elements` neither has a self-sample left,
because every `te[k]` in them became a raw f64 load. The bill that remains is
ucrtbase's `sin`/`cos`/`fmod`/`remainder` at ~28% of samples — the trig
reduction chunk 31 refused to fake — and the fused loop body at 29%.

**And it buys nothing on the other two.** Both deltas are inside their own
arms' layout spread, which is what a re-take under layout control was for: the
older table below records `--pins` at −3.4% on `mesh_churn_2k` and −0.2% on
`instanced_mesh_churn` from single-link A/Bs, and neither survives nine orders.
The census explains the asymmetry rather than excusing it — on
`instanced_mesh_churn` 83 of 155 entries are `@observed` (some store to a field
of that name goes through a receiver the compiler cannot type) and are dropped,
and what survives is `Material`/`Texture` scalars written once at setup, not
anything the hot loop reads.

Two caveats on the table. A pinned build can emit FEWER partition objects than
its default twin (`three_math`: two against three, `mesh_churn_2k`: nine against
ten), so the pinned arm draws from a smaller set of link orders and its spread
column is not directly comparable to the default arm's — the decision rule uses
the wider of the two, which is the default arm's in every row here. And the
`bronze default` column above was re-read in these same sweeps rather than
copied from the table further up, so each row's delta is internal to one
session; `mesh_churn_2k` in particular read 33.5 here against the 35.50 that
table reports.

### Older rows, not re-taken under layout control

Measured 2026-08-27 at `8db7d05`, 51 rounds, `bench/tools/interleave.py`, one
link order per column. The **bronze default** column predates the change that
made `*`, `-`, `/` and `%` over unproven operands produce an f64 in a
standalone build rather than a boxed value, and the three library rows above
have since moved by more than half. Treat every cell below as a record of that
session: re-take it with `layout_sweep.py` before quoting a number, and read
any delta narrower than a few percent as unresolved rather than real.

| fixture | bronze default | bronze `--pins` | node | best vs node |
|---|---:|---:|---:|---:|
| `fib` | **3.64** | 3.66 | 6.08 | **1.67×** |
| `numeric_loop` | **28.27** | 28.33 | 28.96 | **1.02×** |
| `three_math` | 35.60 | **12.52** | 8.80 | 0.70× |
| `object_graph` | 40.29 | **40.06** | 32.28 | 0.81× |
| `typed_array_crunch` | **29.57** | 29.63 | 19.06 | 0.64× |
| `mesh_churn_2k` | 62.68 | **59.96** | 32.33 | 0.54× |
| `instanced_mesh_churn` | 89.64 | **89.49** | 33.61 | 0.38× |
| `property_access` | 4.20 | **4.18** | 1.06 | 0.25× |
| `typed_array_loop` typed | **7.00** | 7.02 | 2.67 | 0.38× |
| `typed_array_loop` plain | **14.19** | 20.29 | 2.39 | 0.17× |
| `proto_dispatch` | **16.61** | 16.82 | 1.64 | 0.10× |
| `proto_dispatch_churn` | **47.96** | 48.36 | 3.13 | 0.065× |

**bronze wins two of the twelve regions above on compute** — `fib` and
`numeric_loop`. Measured as whole processes the same day, the same fixtures
came out **nine wins of eleven** (`instanced_mesh_churn` and
`proto_dispatch_churn` were the only losses; eleven and not twelve because
`typed_array_loop` was one stopwatch reading rather than two regions). The
difference between those two sentences is entirely this suite's protocol
change: a bronze exe starts in ~5.9 ms and node in ~31, so a process stopwatch
hands bronze a 25 ms head start on every row and buries gaps the size of the
ones above. The startup advantage is real and it is worth
having — it is what an AOT compiler is for — but it is not compute, and this
file no longer reports it as though it were.

`proto_dispatch` and `proto_dispatch_churn` had their loops lifted out of top
level into a function so the clock could bracket them (and so the sampler has a
frame to name — at top level everything is `bronze_main`). That is a change to
what gets compiled, so it was checked rather than assumed: both land within
0.3 ms of what the same loops measured at top level under the old protocol once
its startup is subtracted.

Three things this table says that the previous protocol hid:

- **`--pins` is doing much less than it appeared to, everywhere except
  `three_math`.** It is worth 23 ms there and under 3 ms on every other row —
  and on `typed_array_loop.plain` it is worth **−6 ms**, a 43% REGRESSION from
  two `return <fn>: number` lines the census wrote. That is a bug in something,
  not a tuning parameter; the hot-loop IR is unchanged and the pin barrier is
  not the cause.
- **The prototype rows are the largest gaps in the suite** and neither IC
  misses nor helper calls explain them: `instanced_mesh_churn` makes 15,034
  helper calls and 2,663 property-cache misses in its whole run.
- **`typed_array_loop`'s two halves have swapped places.** The typed array is
  now the FASTER half under bronze (7.0 vs 14.2) and the slower half under node
  (2.67 vs 2.39) — bronze's typed-element work landed, and its plain-array
  element path is what is left behind.

## Reproducing

For anything built out of a library — the three rows in the table above, and
any A/B whose expected delta is smaller than about 10% — use the layout sweep,
because a single link order is one draw of a distribution several percent wide:

```sh
# 1. compile each arm ONCE; the objects, not the exe, are the artefact
bronze build bench/instanced_mesh_churn.js -o /tmp/base.exe --keep-objs /tmp/objs/base
bronze build path/to/edited/entry.js      -o /tmp/edit.exe --keep-objs /tmp/objs/edit

# 2. a spec naming the object directories, the seeds, and the rounds
#    (`null` in "seeds" is the default order, so the table shows where
#    today's number sits in the distribution as well as how wide it is)
cat > sweep.json <<'JSON'
{"region": "instanced_mesh_churn",
 "seeds": [null, 1, 2, 3, 4, 5, 6, 7, 8], "rounds": 11, "copies": 3,
 "bronze": "<abs-path>/bronze.exe", "launcher": ["<abs-path>/dev.cmd"],
 "stage": "/tmp/stage",
 "arms": [{"name": "base", "objs": "/tmp/objs/base"},
          {"name": "edit", "objs": "/tmp/objs/edit"}]}
JSON

# 3. link every (arm, seed), run them interleaved, apply the decision rule
python bench/tools/layout_sweep.py sweep.json
```

For a kernel that compiles to one object there is no order to permute, and
`interleave.py` is still the instrument — it is also the only one that can take
a node column:

```sh
# 1. build the columns you want to compare
bronze build bench/three_math.js -o tm_def.exe
bronze build bench/three_math.js -o tm_census.exe --census tm.pins && ./tm_census.exe
grep -v '@observed' tm.pins > tm_safe.pins
bronze build bench/three_math.js -o tm_pin.exe --pins tm_safe.pins

# 2. write a spec naming them, plus a node column taken in the SAME session
cat > spec.json <<'JSON'
[{"name": "default", "exe": "./tm_def.exe"},
 {"name": "pins",    "exe": "./tm_pin.exe"},
 {"name": "node",    "js":  "<abs-path>/bench/three_math.js"}]
JSON

# 3. interleave
python bench/tools/interleave.py spec.json 51
```

A column with `"js"` runs under node and exists so a node number is taken
alongside the bronze ones rather than remembered from another session. Give it
an ABSOLUTE, native path: node resolves `bench/package.json`'s `"type":
"module"` from the file's own directory, and on Windows it will not accept an
MSYS `/d/...` path at all. The
automated runner never emits one — `node` is not a dependency of any build or
test (CLAUDE.md) — and `interleave.py` is the manual instrument where that
comparison is made.
