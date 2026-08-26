# Per-slot representation (stages R1 and R2)

A property slot used to hold exactly one thing: a NaN-boxed `Value`. Uniform,
and the reason double-heavy code pays a tag at every property boundary —
three.js's `Vector3`, `Matrix4` and `Quaternion` spend their whole lives moving
doubles through slots whose contents are only known at run time.

Stage R1 gives a **shape** the ability to say, per slot, that the slot's eight
bytes **are a double**: readable as an `f64` with no tag test and no branch.
What R1 lands is the storage model, the machinery that keeps the promise true,
and the measurement that says which other slots are worth claiming. **Stage R2**
is what generated code spends it on, and it is the second half of this document.

Implementation: `src/runtime/slot_repr.{h,cpp}`, `src/runtime/shape.{h,cpp}`
(`double_slots`, `repr`, `withSlotBoxed`), `src/runtime/object.h`
(`ObjectHeader::setSlot`), `src/runtime/heap.cpp` (`scan_plain_object`);
stage R2 in `src/codegen-llvm/llvm_repr.{h,cpp}` and the store sites it feeds.

## The claim, and how it is kept

**A double slot is created only from a promise.** A shape node is born
`SlotRepr::Double` when the store that creates the property is a Number *and*
the property's name is on the eligibility list the module handed over at init
(`bronze_register_slot_repr`) — which the compiler builds from the `--pins`
manifest's `number` fields on classes whose layout it proved. No pins manifest,
no double slots.

**A double slot is taken back by a shape change.** bronze has no deopt. When a
non-Number reaches a double slot, `ObjectHeader::setSlot` moves *that object* to
a shape rebuilt with the slot boxed (`Shape::withSlotBoxed`). Shape nodes are
immutable once created, so every other object still at the old shape is
untouched and still holds a double there; compiled guards keyed on the old shape
simply stop matching for the object that moved. The demotion is **sticky** — the
double node is marked, so the next object to install that key on that parent
takes the boxed edge, and a field that turns over splits the shape tree once
rather than once per object.

**Every write goes through one place, or tests before it writes.**
`ObjectHeader::setSlot` is the choke point, and the runtime's every store path
calls it — `bronze_prop_set`, `defineProperty`, `Object.assign`, spread, the
dictionary conversion, `delete`. Generated code's bare stores cannot route
through a function, so each one makes the same test inline:

| path | test |
|---|---|
| set-site inline cache | an entry naming a double slot carries `BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG`, and the arm takes it only when the value is a Number. Both arms: the own-property store and the transition store a constructor's `this.x = x` runs on |
| static-slot site, identity form | the shape word the guard already loaded carries `double_slots`; the store ands it with its compile-time slot's bit and requires a Number when it is set |
| static-slot site, family form | the same test, and the reason it lives in the site rather than in `classFamilyIdFor`: a family guard has no per-site hook that could refuse only the stores |

A miss costs one helper call per *field*, not per store: `setSlot` generalizes
the slot, and the cache entry refilled against the new shape has no flag left to
test.

**The one store that misses repeatedly** is an `Int32`-tagged value — what
lowering boxes an `il::Type::I32` into, so `this.n = i | 0` and friends. Its
bits are a tag and a payload, not an f64, so the arms refuse it and `setSlot`
converts; the slot stays a double one, so the *next* such store misses too. It
is correct, and it is a helper call per store for a field written that way.

Stage R2 converts inline instead (`sitofp` and a select), and pays for it only
where the compiler has already proved the stored value is an `Int32` — see
[the Int32 arm](#the-int32-arm-and-why-it-has-no-sites) below, which also records
that no JavaScript in the corpus currently reaches it.

`Heap::verify_space` (under `BRONZE_HEAP_VERIFY=1`) is the tripwire for a write
that got past all of them: every slot a shape calls a double must hold a Number,
and a violation names the object and the slot.

## Why reads still hit in stage R1

bronze NaN-boxes *directly*: a Number's `Value` bits are the double's bits. A
double slot is written NaN-canonicalized, so the word it holds is also the boxed
`Value` for that number. Every reader that has not yet learned about
representations — an inline cache's slot load, a static-slot site's
constant-offset load, the collector's generic payload scan — keeps giving the
right answer. That is what lets the storage model land without teaching the
codegen anything about representations: the read half of a pinned field is
untouched, and the write half needs a test rather than a conversion.

**Stage R2 kept the canonicalization**, and the forward-looking note that used
to stand here — that R2 would stop — was wrong about which half of the trade is
worth having. Canonicalizing is what keeps a double slot's word a legal `Value`
for every reader that does not know about representations: `bronze_prop_get`,
`JSON.stringify`, `Object.is`, the accessor halves, the dictionary conversion,
`ensureOverflow`'s block copy, `Heap::verify_space`, the census. Dropping it
would have made all of those conditional on a shape lookup in order to save one
`fcmp uno` and one `select` per store — and it is that same identity, "a Number's
box IS the double", that makes stage R2's raw store a store of *the bits it
already had* rather than a conversion. The compatibility turned out to be the
mechanism, not scaffolding around it.

## What the collector does

`Heap::scan_plain_object` reads the object's shape, skips the slots
`double_slots` names, and scans the object's out-of-line slot block itself —
the block carries `HeapKind::SlotBlock` and the generic pass skips it, because
only the owner knows which of its words are Values.

Two heap kinds were added for this, and they are the price of `HeapKind::Plain`
becoming a *claim* that a `Shape*` sits at offset 8:

* `HeapKind::SlotBlock` — an object's out-of-line property slots.
* `HeapKind::ValueBlock` — an array's elements, a Map's entry table: flat runs
  of Values that used to be left at the zero `Heap::allocate` writes, which
  reads as `Plain`.

`FunctionHeader::create` now sets its own kind too, closing the few-line window
between the allocation and the caller's assignment. A header that says `Plain`
and is not an object is a named fatal, not a fault somewhere else.

## Env vars

| var | effect |
|---|---|
| `BRONZE_NO_SLOT_REPR=1` | **the seam.** No shape node is ever created double, every `double_slots` word stays zero, and storage is exactly what it was before the stage. |
| `BRONZE_SLOT_REPR_OBSERVED=1` | every key becomes eligible, not only the pinned ones — the unpinned "first store was a double" policy. Implemented, **off by default**: an unpinned program has no promise to hold its store paths to, and a name that alternates costs a shape split each time it turns over. |
| `BRONZE_SLOT_REPR_STATS=1` | prints the creation-side counters at exit: eligible names, shape nodes born double vs boxed, number stores refused for an ineligible name, generalizations, and the stores that reached a double slot through the helper. Costs a normal run nothing — every counter is incremented on a cold path. |
| `BRONZE_SLOT_REPR_CENSUS=1` | adds **per-(shape, slot) representation stability**: for every slot of every shape the run touched, how many stores were Numbers, how many were not, and how many reads. Turns on the shape census's latch suppression (`docs/shape-census.md`) so that inline-cache hit traffic is visible, so a census run is **counts, never times**. |

`BRONZE_SLOT_REPR_STATS=1` on `bench/three_math.js`, compiled with
`bench/pins/threejs-math.pins`:

```
=== slot representation (stage R1) ===
  seam            : on
  eligible names  : 7
  shape nodes     : 10 double, 328 boxed
  refused         : 20 (number store, name not eligible)
  generalizations : 0 stores over 0 nodes
  helper stores   : 40 into a double slot
```

Ten double slots: `Vector3.x/y/z`, `Quaternion._x/_y/_z/_w`, `Euler._x/_y/_z`.
Zero generalizations — the manifest's promises hold, which is what the entries
in `bench/pins/threejs-math.pins` claim and what a census run checks rather than
assumes.

Forty helper stores out of the run's ~660,000 stores into those slots, and the
small number is the good news: the other 659,960 took an inline arm, tested the
value and stored it there. `helper stores` counts what reached `setSlot`, so it
reads near zero exactly when the sites have latched. What says the slots are
being made is `shape nodes`.

## The R2 planning number

`BRONZE_SLOT_REPR_CENSUS=1` on the same run prints a row per (shape, slot) and
one summary line. On `three_math` the rows are the manifest, checked:

```
  shape                        slot key                 num    other    reads  repr
  plain{z,y,x}                 0    x                150000        0   150000  double
  plain{z,y,x}                 1    y                150000        0   150000  double
  plain{z,y,x}                 2    z                150000        0   150000  double
  plain{elements}              0    elements              0        0   300000  boxed
  plain{_order,_z,_y}          3    _z                30000        0    30001  double
  plain{_order,_z,_y}          4    _order                0    30000    30001  boxed
```

Ten slots with a six-figure number-store count and a zero beside it, and
`_order` — a string, correctly not pinned and correctly boxed — as the control.
A **double** slot with any non-number store would be flagged `<-- VIOLATED` on
its row: that is a manifest entry the run disproved.

The last line is the one stage R2 reads:

```
  boxed slots whose every store was a number: N (M accesses) — stage R2's candidate set
```

A boxed slot whose every observed store was a Number is a slot the next stage
could claim, either by widening the manifest or by turning
`BRONZE_SLOT_REPR_OBSERVED` into a policy with a proof behind it. `three_math`
under its full manifest answers **0** — there is nothing left to claim, which is
what a complete manifest looks like from here. `bench/object_graph.js`, which has
no manifest at all, answers **11 slots over 5,098,196 accesses**: `x/y/z`,
`worldX/worldY/worldZ`, `visited`, `sumX/sumY/sumZ`. That is the shape of the
question R2 opens with.

---

# Stage R2: what generated code spends

R1 made the storage model true. R2 is the compile-time half: a fact about an IL
**value**, spent at the places R1 had to leave a test.

Implementation: `src/codegen-llvm/llvm_repr.{h,cpp}` (the plan),
`llvm_static_slot.cpp` and `llvm_prop_set.cpp` (the store arms),
`llvm_frame.cpp` (the roots). Tests: `tests/codegen_llvm/repr_test.cpp`,
`tests/oracle/cases/slot_repr_codegen.js`.

## The plan

`planRepr(const il::Function&)` answers, for every `dynamic` SSA value, what its
eight bytes are made of. Four answers, and the two that matter are not the same
question:

| answer | means | who asks |
|---|---|---|
| `Number` | `bits <= NUMBER_MAX` — the bits **are** an IEEE double, NaN canonicalized | the store arms |
| `Int32Boxed` | `Tag::Int32` — a Number by 6.1.6.1, but a tag and a payload by the bits | the store arms |
| `NotPointer` | not a heap address, whatever else it is | the GC root frame |
| `Unknown` | anything. R1's answer for everything | — |

It is a pure function of the IL, computed for every function before any body is
emitted, so it is testable against IL text rather than against generated code.

Three rules produce every non-`Unknown` answer:

* **A `box.f64` is a `Number`**, because the emitter's canonicalizing select is
  exactly the promise. `box.i32` is `Int32Boxed`, `box.bool` / `const.undefined`
  / `const.null` are `NotPointer`, and `box.str` is a heap pointer and stays
  `Unknown` — the one Box that must not be mistaken for its neighbours.
* **A value every one of whose uses is a RAW `unbox.f64` is a `Number`.** That
  claim is already granted by `provenFieldReads` or by a `--pins` entry, and
  what it licenses is a bare bitcast to double; spending the same claim on the
  root is not a second assumption. All uses, so a value read once raw and once
  as a `Value` keeps its slot. `nullishUnbox` gives `NotPointer`.
* **A block parameter is the meet of its incoming edges**, computed as a
  **greatest fixpoint**: every value starts above the lattice and the rules only
  lower it. A loop-carried value is defined in terms of an edge defined in terms
  of it, and only a greatest fixpoint says the true thing about that circle. The
  top element is a fifth state and deliberately not `Number` — meeting a fresh
  value with an `Int32Boxed` rule has to give `Int32Boxed`, and starting at
  `Number` would collapse it to their disagreement.

## The raw store, and why it needs no guard of its own

At a store site whose guard has established the receiver's shape, R1 loaded
`double_slots`, masked the site's compile-time slot bit, tested the value for
Number, and branched to a miss when the two disagreed. R2 emits **none of that**
when the value is a proven `Number`:

> A Number's box IS the canonical double a double slot promises. The same eight
> bytes are the correct contents of a double slot and of a boxed slot. So the
> store is right whichever way the bit reads, and there is nothing left to ask.

That is the whole soundness argument, and it is stronger than a guard: it does
not depend on the slot's representation at all, so it survives **generalization
between two visits to the same site**. An object that re-boxed the slot after
the last store is still stored into correctly, and the shape-mate that did not
generalize is still holding a double. `tests/oracle/cases/slot_repr_codegen.js`
runs that interleaving in both orders.

What is still required is what R1 already required: a **dominating shape guard
on the same receiver value** naming the slot. R2 widened no site's guard
coverage; every raw store is at a site that was already guarded. Accessor slots
are excluded by the guard the site already carries (the IC entry's `ACCESSOR`
flag makes `depthBase != 0`, and a published static-slot cell is only ever filled
for an own data property), and a dictionary-mode object has no shape the guard
can match.

Emitted shapes, from `tests/codegen_llvm/repr_test.cpp`:

| value | loads | `sitofp` | `select` | stores |
|---|---|---|---|---|
| proven `Number` | 3 (the guard's) | 0 | 0 | 1 |
| `Int32Boxed` | 4 | 1 | 1 | 1 |
| `Unknown` (R1's arm) | 4 | 0 | 0 | 1 |

## The roots

`planFrame` gives every `il::Type::Dynamic` value a GC root slot; defs store into
it and **every use reloads out of it**. A root exists so the collector can
forward what moved, and nothing that is not a pointer can move — so a value the
plan calls `NotPointer` needs no slot, which removes its root store *and* the
reload at every use.

This is the larger of R2's two effects on the emitted code, and it is the one
that is invisible from the store sites: `box.f64` results are everywhere, and in
R1 each of them made a round trip through the stack between being computed and
being used.

## The Int32 arm, and why it has no sites

R1 documented an `Int32`-tagged store as its standing cost: `this.n = i | 0`
misses to the helper on every store, forever. R2 implements the inline
conversion — `sitofp` of the payload, selected against the slot's bit — and it
is correct and tested. **It has no sites.**

`emitToInt32` types its result `il::Type::I32`, and every bitwise operator
converts that result straight back to `F64` (`lower_expr_binary.cpp` says why:
the type lattice has no int32 element, so an I32 crossing a block edge would
fail to coerce). So the IL for `this.n = i | 0` is `to.int32` then `f64` then
`box.f64`, and `box.i32` reaches a property store nowhere in `bench/`,
`tests/oracle/cases/` or `tests/oracle/threejs/`. Measured, not assumed:
`BRONZE_REPR_CODEGEN_STATS=1` reports `sitofpStores=0` on every one of them.

The arms are kept because they are the correct answer for the IL shape and are
pinned by unit tests against hand-built IL; the *gap R1 named does not exist* in
today's lowering, which is the finding.

## Two negative results

**The get side was not built.** The plan for R2 called for a raw load at a get
site whose guard names a double slot, mirroring `BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG`
on the set side. Two measurements said not to:

* Because R1 canonicalizes, a double slot's eight bytes **are** the boxed
  Number. A "raw load" is bit-identical to the load already emitted. The only
  thing a get-side representation could remove is one perfectly-predicted `ULE`
  compare at a following unbox — and discovering the representation costs a
  load, a mask, a compare and a branch.
* In `bench/three_math.js` compiled with its manifest there are 108 raw
  `unbox.f64` and 142 checked ones, and **zero** checked `unbox.f64` whose
  operand is a static-slot `prop.get`. The pin proof and the layout proof
  coincide exactly, so there is no site where the flag would have said anything
  the IL did not already say.

Recorded as R3 input rather than built.

**The unboxed-dataflow half is already done, by the typing.** R2 was to keep a
raw-loaded f64 in an FP register through arithmetic instead of re-boxing between
links. Under `--pins` that is already the emitted code: the IL is
`unbox.f64 raw` then `f64` arithmetic then `box.f64` then the raw store, with no
tag test anywhere in it. An emitter arm for `dynamic` arithmetic over two proven
Numbers was written, measured to have **zero sites in every kernel and in a probe
built to provoke it**, and removed rather than shipped — `lower_infer` gives
those chains `il::Type::F64` and they never reach a boxed operator at all.

## Env vars

| var | effect |
|---|---|
| `BRONZE_NO_REPR_CODEGEN=1` | **the stage R2 seam, read by the COMPILER.** Every value comes back `Unknown`, so every site emits exactly the stage R1 sequence and every `dynamic` value keeps its root. Build-time and not run-time deliberately: what it isolates is the emitted code. `tests/oracle/pin_matrix.sh` sweeps it as a `CSEAMS` entry. |
| `BRONZE_REPR_CODEGEN_STATS=1` | prints one line to stderr after the module is emitted: functions planned, values proven `Number`, roots elided, raw store sites, `sitofp` store sites. Counts EMITTED SITES, so an inlined callee's sites count once per region — which is how many of them the binary has. |

```
$ BRONZE_REPR_CODEGEN_STATS=1 bronze build bench/three_math.js --pins bench/pins/threejs-math.pins -o tm.exe
[repr] functions=252 provenNumber=1042 rootsElided=1264 rawStores=604 sitofpStores=0
```

A representation stage is easy to believe in and hard to check: every arm is
conditional on a proof, so a stage that proves nothing emits exactly the code it
replaced and the whole suite still passes. This line is what separates "the fast
arm is correct" from "the fast arm is ever taken".
