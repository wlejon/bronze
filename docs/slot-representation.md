# Per-slot representation (stage R1)

A property slot used to hold exactly one thing: a NaN-boxed `Value`. Uniform,
and the reason double-heavy code pays a tag at every property boundary —
three.js's `Vector3`, `Matrix4` and `Quaternion` spend their whole lives moving
doubles through slots whose contents are only known at run time.

Stage R1 gives a **shape** the ability to say, per slot, that the slot's eight
bytes **are a double**: readable as an `f64` with no tag test and no branch.
Nothing in generated code spends that promise yet — that is stage R2. What R1
lands is the storage model, the machinery that keeps the promise true, and the
measurement that says which other slots are worth claiming.

Implementation: `src/runtime/slot_repr.{h,cpp}`, `src/runtime/shape.{h,cpp}`
(`double_slots`, `repr`, `withSlotBoxed`), `src/runtime/object.h`
(`ObjectHeader::setSlot`), `src/runtime/heap.cpp` (`scan_plain_object`).

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

The compatibility is deliberate and temporary. Stage R2 stops canonicalizing and
starts loading slots as raw `f64`, at which point the collector's precision
below becomes load-bearing and the store-side tests above become a conversion
rather than a guard.

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

The census report's last line is the one stage R2 reads:

```
  boxed slots whose every store was a number: N (M accesses) — stage R2's candidate set
```

A boxed slot whose every observed store was a Number is a slot the next stage
could claim, either by widening the manifest or by turning
`BRONZE_SLOT_REPR_OBSERVED` into a policy with a proof behind it. A **double**
slot with any non-number store is flagged `<-- VIOLATED` on its row: that is a
manifest entry the run disproved.
