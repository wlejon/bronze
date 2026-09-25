# Per-slot representation

A property slot holds a NaN-boxed `Value` unless its shape says otherwise.
Uniform boxing is the reason double-heavy code pays a tag at every property
boundary — three.js's `Vector3`, `Matrix4` and `Quaternion` spend their whole
lives moving doubles through slots whose contents are only known at run time.

A **shape** can say, per slot, that the slot's eight bytes **are a double**:
readable as an `f64` with no tag test and no branch. This document covers the
storage model, the machinery that keeps the promise true, and the measurement
that says which other slots are worth claiming.

Implementation: `src/runtime/slot_repr.{h,cpp}`, `src/runtime/shape.{h,cpp}`
(`double_slots`, `repr`, `withSlotBoxed`), `src/runtime/object.h`
(`ObjectHeader::setSlot`), `src/runtime/heap_trace.cpp` (`traceCell`).

## The claim, and how it is kept

**A double slot is created only from a promise.** A shape node is born
`SlotRepr::Double` when the store that creates the property is a Number *and*
the property's name is on the eligibility list the module handed over at init
(`bronze_register_slot_repr`) — which the compiler builds from the `--pins`
manifest's `number` fields on classes whose layout it proved. No pins manifest,
no double slots.

**A double slot is taken back by a shape change.** When a non-Number reaches a
double slot, `ObjectHeader::setSlot` moves *that object* to a shape rebuilt with
the slot boxed (`Shape::withSlotBoxed`). Shape nodes are immutable once created,
so every other object still at the old shape is untouched and still holds a
double there; inline caches keyed on the old shape simply stop matching for the
object that moved. The demotion is **sticky** — the double node is marked, so
the next object to install that key on that parent takes the boxed edge, and a
field that turns over splits the shape tree once rather than once per object.

**Every write goes through one place.** `ObjectHeader::setSlot` is the choke
point, and every store path calls it — `bronze_prop_set` (the store compiled
code's `prop.set` lowers to), `defineProperty`, `Object.assign`, spread, the
dictionary conversion, `delete`. A set-site inline cache entry naming a double
slot carries `BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG` (`ICEntry::isDoubleSlot`), the
test an inline store arm in generated code would have to make before taking the
entry. The brass backend emits no inline store arm — `prop.set` is a call to
`bronze_prop_set` — and the runtime's store path ignores the flag, because it
reaches `setSlot` in every case.

An `Int32`-tagged value — what lowering boxes an `il::Type::I32` into — is a
Number whose bits are a tag and a payload, not an f64, so `setSlot` converts it
and the slot stays a double one. `emitToInt32` types its result `il::Type::I32`,
and every bitwise operator converts that result straight back to `F64`
(`lower_expr_binary.cpp` says why), so `this.n = i | 0` stores a `box.f64`.

`Heap::verify_space` (under `BRONZE_HEAP_VERIFY=1`) is the tripwire for a write
that got past `setSlot`: every slot a shape calls a double must hold a Number,
and a violation names the object and the slot.

## Why reads need nothing

bronze NaN-boxes *directly*: a Number's `Value` bits are the double's bits. A
double slot is written NaN-canonicalized, so the word it holds is also the boxed
`Value` for that number. Every reader that does not know about representations —
an inline cache's slot load in compiled code, the collector's generic payload
scan — gives the right answer. The read half of a pinned field is untouched, and
the write half needs a test rather than a conversion.

Canonicalizing is what keeps a double slot's word a legal `Value` for every
reader that does not know about representations: `bronze_prop_get`,
`JSON.stringify`, `Object.is`, the accessor halves, the dictionary conversion,
`ensureOverflow`'s block copy, the collector's trace and verifier, the census.
Without it all of those would be conditional on a shape lookup in order to save
one `fcmp uno` and one `select` per store.

## What the collector does

Nothing representation-specific. A double slot's word is a canonical boxed
Number, which carries no pointer tag, so the trace (`traceCell`) visits every
word after an object's shape pointer — inline slots, and the out-of-line slot
block as a cell of its own — without consulting `double_slots`; brass's
collector leaves a word that is not a reference alone.

The heap kinds below are what makes `HeapKind::Plain` a *claim* that a
`Shape*` sits at offset 8, which the trace relies on to skip that word:

* `HeapKind::SlotBlock` — an object's out-of-line property slots.
* `HeapKind::ValueBlock` — an array's elements, a Map's entry table: flat runs
  of Values.

`FunctionHeader::create` sets its own kind, so there is no window between the
allocation and the caller's assignment. A header that says `Plain` and is not an
object is a named fatal, not a fault somewhere else.

## Env vars

| var | effect |
|---|---|
| `BRONZE_NO_SLOT_REPR=1` | **the seam.** No shape node is ever created double, and every `double_slots` word stays zero. |
| `BRONZE_SLOT_REPR_OBSERVED=1` | every key becomes eligible, not only the pinned ones — the unpinned "first store was a double" policy. Implemented, **off by default**: an unpinned program has no promise to hold its store paths to, and a name that alternates costs a shape split each time it turns over. |
| `BRONZE_SLOT_REPR_STATS=1` | prints the creation-side counters at exit: eligible names, shape nodes born double vs boxed, number stores refused for an ineligible name, generalizations, and the stores that reached a double slot through the helper. Costs a normal run nothing — every counter is incremented on a cold path. |
| `BRONZE_SLOT_REPR_CENSUS=1` | adds **per-(shape, slot) representation stability**: for every slot of every shape the run touched, how many stores were Numbers, how many were not, and how many reads. Turns on the shape census's latch suppression (`docs/shape-census.md`) so that inline-cache hit traffic is visible, so a census run is **counts, never times**. |

`BRONZE_SLOT_REPR_STATS=1` on `bench/three_math.js`, compiled with
`bench/pins/threejs-math.pins`:

```
=== slot representation ===
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

`helper stores` counts what reached `setSlot` with a double slot as the target,
so it reads near zero exactly when the set sites have latched onto the double
arm. What says the slots are being made is `shape nodes`.

## The census

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

Slots with a six-figure number-store count and a zero beside it, and `_order` —
a string, correctly not pinned and correctly boxed — as the control. A
**double** slot with any non-number store would be flagged `<-- VIOLATED` on its
row: that is a manifest entry the run disproved.

The last line is the candidate set for widening the manifest:

```
  boxed slots whose every store was a number: N (M accesses) — candidates for a wider manifest
```

A boxed slot whose every observed store was a Number is a slot a wider manifest,
or `BRONZE_SLOT_REPR_OBSERVED` turned into a policy with a proof behind it,
could claim. `three_math` under its full manifest answers **0** — there is
nothing left to claim, which is what a complete manifest looks like from here.
`bench/object_graph.js`, which has no manifest at all, answers **11 slots over
5,098,196 accesses**: `x/y/z`, `worldX/worldY/worldZ`, `visited`,
`sumX/sumY/sumZ`.
