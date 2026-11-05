# The shape-flow census (`BRONZE_SHAPE_CENSUS=1`) — artifact schema v0

The census is the measurement half of a future whole-program monomorphizer:
per property-access site, which receiver shapes actually flow through it, at
what polymorphism degree, and how much of its value traffic is numbers (the
unboxing opportunity). Its JSON artifact is the **input contract** for that
monomorphizer, so the schema is versioned and every field is documented here.
Additions bump the minor meaning of the version string; removals or meaning
changes are a new version.

Implementation: `src/runtime/shape_census.{h,cpp}`; the latch suppressions it
depends on live in `thread_seams.cpp` (TLS seam words), `object.cpp` /
`rt_prop_absent.cpp` (property-IC fills), `rt_method_call.cpp` (method-IC
latch), `static_shape.cpp` (static publish), `class_family.cpp` (family
stamp).

## How it measures

Census mode disables every inline-cache **latch** in the runtime, so every
property access that would have hit an inline fast path keeps missing into
its runtime helper, where the census records it. That gives full-traffic
visibility — including the monomorphic-hit traffic a miss logger
(`BRONZE_IC_LOG`) structurally cannot see — with **no codegen change and no
ABI change**. The price is a heavy slowdown in census mode (every access pays
a helper call plus a full lookup plus the recording); numbers from a census
run are **counts, never times**. Pair them with `BRONZE_SAMPLE` timings from
an uninstrumented run.

With the env var unset, every hook is one predicted branch on a global; the
disabled-mode cost is bench-checked at ≤ noise (`bench/tools/interleave.py`
over the pure-compute fixtures, census on against off).

## What a "site" is

* Named reads (`o.k`), named writes (`o.k = v`) and method fetches
  (`o.m(...)`) are keyed by their **IC-site address** — module data, one per
  compiled source site, stable for the process lifetime.
* Computed reads/writes (`o[k]`) and `super.k` accesses have no per-site IC;
  they are keyed by the **helper call's return address**, which is unique per
  call site up to backend block merging.
* Function attribution: the site's return address is symbolized through the
  module PDBs at dump time, so `fn` carries the same verbatim IL names the
  sampler reports (`Matrix4.multiplyMatrices`, `__anon_fn_1354`,
  `main.seg3`), and the two artifacts join on that string.

## Known coverage limits (v0)

* Array/typed-array **element** fast paths (`a[i]` with a numeric in-range
  index on a receiver the tag guards accept) are emitted inline without a
  runtime seam and stay invisible. They carry no shape information and are
  not monomorphization targets.
* A getter that runs user JS *during a method fetch* has its inner accesses
  suppressed (the method helper brackets its internal property read as
  nested). Methods are data properties in practice; the loss is noise.
* `receiver shape` for non-plain receivers is a **pseudo-shape** naming the
  receiver kind (`array`, `string`, `typedarray`, ...). Poly degree therefore
  means "distinct layouts" for plain objects and "distinct kinds" otherwise.
* Sites whose observations are all from runtime-internal callers (builtins
  reaching helpers directly) attribute to the runtime module, not to a JS
  function.

## Monomorphic-in-practice

A site is *monomorphic-in-practice* when its dominant receiver identity
covers **≥ 99%** of its observations over the whole run (`mono_threshold` in
the artifact). Whole-run dominance subsumes cross-frame stability: a site
that flips shape between frames cannot reach 99% over hundreds of frames.

## Schema (`bronze-shape-census-v0`)

```json
{
  "version": "bronze-shape-census-v0",
  "mono_threshold": 0.99,
  "total_observations": 123,     // sum of hits over all sites
  "total_sites": 45,
  "mono_observations": 100,      // observations at monomorphic-in-practice sites
  "poly_histogram": {            // sites and observations bucketed by poly degree
    "1": {"sites": 1, "observations": 2}, "2": {...}, "3": {...},
    "4": {...}, "5-8": {...}, ">8": {...}
  },
  "sites": [ { ...site... } ],   // sorted by hits, descending
  "functions": [ { ...fn... } ], // per-function aggregation, sorted by observations
  "t0_ms": 33022683,             // the steady clock (CLOCK_MONOTONIC) at census start, ms
  "leave_total": 240631,         // shape leaves over the run
  "leaves": [ { ...leave... } ], // see "Leaves"
  "epoch_bumps": [ { ...bump... } ]
}
```

### Site fields

| field | meaning |
|---|---|
| `id` | site key (IC-site address or return address), stable within one run only |
| `kind` | `get`, `set`, `elem_get`, `elem_set`, `method_get`, `super_get`, `super_set` |
| `key` | the interned property name for named sites; empty for computed sites |
| `fn` | symbolized function containing the access (PDB name, joins with sampler) |
| `module` | module the site's code is in (`app.dll` = compiled JS) |
| `hits` | observations at this site |
| `poly_degree` | distinct receiver identities seen |
| `top_share` | dominant identity's share of `hits` |
| `mono` | `top_share >= mono_threshold` |
| `number_vals` | reads whose RESULT, or writes whose STORED VALUE, was a number (double or Int32 tag) — the unboxing-opportunity proxy |
| `undefined_vals`, `bool_vals`, `object_vals`, `string_vals`, `other_vals` | remaining value-tag traffic |
| `transitions` | writes whose key was absent from the receiver's shape (the store transitions or dictionary-adds) |
| `key_num_index` / `key_num_other` / `key_symbol` | computed sites: key-kind counts |
| `ic` | the site's index in its module's IC table (`__bronze_ic_table`), or -1 for a site keyed by return address; what `--wpo-stats` prints as `ic N` (see "Joining with the build"). Only meaningful on rows whose `module` is the compiled module: a native's site is resolved against the nearest table below it |
| `shapes` | up to 16 `{shape, count, desc}` rows, largest first; `desc` is the shape's nearest three own keys (`plain{x,y,z}`) or a receiver-kind name |
| `computed_keys` | computed sites: up to 16 distinct string keys with counts, overflow under `"(other)"` |

### Function fields

| field | meaning |
|---|---|
| `name` / `module` | symbolized function; same spelling as the sampler's rows |
| `sites` | census sites attributed to it |
| `observations` | sum of its sites' hits |
| `mono_observations` | hits at its monomorphic-in-practice sites |
| `coverage` | `mono_observations / observations` — the fraction of this function's dynamic property traffic a shape-monomorphizing compiler could specialize without a polymorphic guard |
| `number_vals` | summed number-value traffic |
| `transitions` | summed transitioning writes |

## Leaves

Every move of an object AWAY from a shape it had (runtime/shape_leave.h): an
append (`add`), a double slot widened (`generalize`), a dictionary conversion
by `delete`, `redefine` (an attribute change or redefinition the chain cannot
express), `integrity` (freeze / seal / preventExtensions), `set_prototype` or
`threshold` (the add past 1024 properties), and `become_prototype` (a move to a
private marked chain). Appends are counted at the write that finds its key
absent (named and string-keyed writes; a `defineProperty` add is not seen), the
rest where the runtime moves the object. Census mode suppresses every compiled
transition, so no append escapes into generated code.

| field | meaning |
|---|---|
| `shape`, `desc` | the shape left, as in a site's `shapes` row |
| `total` | leaves of it over the run |
| `read_hits` | named reads and method fetches that observed it (0: no site reads through it, e.g. a constructor's intermediate shape) |
| `top_reader` | the function of the busiest such site |
| `kinds` | leaves by kind |
| `added` | for appends, the keys added (8 named, the rest under `(other)`) |
| `seconds` | `[second, leaves]` pairs, seconds since `t0_ms` |

Rows of shapes some site reads through come first, by `read_hits`; at most
2000 unread rows follow. A measured window in CLOCK_MONOTONIC milliseconds (the
`traceWindow` bro's bench harness reports) selects the steady-state leaves:
the seconds `s` with `t0_ms + 1000 s` inside it.

`epoch_bumps` counts prototype-epoch bumps (`bumpProtoMutationEpoch`, which is
also the layout epoch: docs/aot-specialization.md, "S1") as `{caller,
seconds}`, the caller symbolized.

## Joining with the build

`bronze build --wpo-stats=<fn>[,<fn>...]` prints, for the named functions,
every keyed site (`wpo-pred-site ... ic N r%V`: the IC index and the receiver's
SSA value) and one `wpo-pred-seq <fn>` line of the function's keyed sites and
calls in block order. `bench/tools/census_receivers.py <census.json>
<wpo.txt> <fn,...>` joins the two on the IC index: per receiver, accesses per
activation, distinct shapes, the predicting solve's classes, and the calls and
IC stubs between consecutive accesses. Build the `--wpo-stats` copy with the
census run's exact command line, so the indices agree.

## The go/no-go number

The campaign's monomorphization decision input is: **the fraction of inline
compiled-code time spent in functions whose property traffic is
monomorphic-in-practice.** It joins the two artifacts on function name:

```
sum over fn of (sampler self_tail ms of fn in app.dll × census coverage(fn))
────────────────────────────────────────────────────────────────────────────
             sum over fn of (sampler self_tail ms of fn in app.dll)
```

computed by `bench/tools/census_join.mjs` (manual analysis tooling, never run
by the automated bench runner).

## Env vars

| var | effect |
|---|---|
| `BRONZE_SHAPE_CENSUS=1` | enable census mode (latch suppression + recording + dump at exit) |
| `BRONZE_SHAPE_CENSUS_OUT=path` | artifact path, default `bronze_shape_census.json` in the CWD; also carries the leave log and the epoch bumps |
| `BRONZE_SLOT_REPR_CENSUS=1` | the sibling mode: per-(shape, slot) representation stability, printed to stderr at exit. It turns the suppression above on for its own reasons — an inline-cache hit is traffic it has to see — so a run with it set is a census run in every other respect. [docs/slot-representation.md](slot-representation.md) has its report and the candidate count it gives for widening a pins manifest. |

A human summary (poly-degree histogram, monomorphic-coverage line, top
functions) is printed to stderr at exit alongside the artifact.
