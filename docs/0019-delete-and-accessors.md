# 0019 — Delete, dictionaries, and properties that are not slots

Status: implemented. Part of phase 4 of docs/0001.

Two operators, one consequence. `delete o.k` removes a property, and
`get x() {}` / `set x(v) {}` define one that is a pair of functions rather
than a value. Neither is hard to parse. Both break the same assumption, which
every layer underneath had been built on since docs/0004:

> an object's own properties are the nodes of its shape chain, and a property
> is a slot index you can load from.

`delete` breaks the first half — a chain is the wrong structure to remove
from the middle of. An accessor breaks the second — the answer is not in the
slot, it is what happens when you call what is in the slot. And the inline
caches of docs/0010 decision 7 are built on **both** halves at once: an entry
says "for this shape, load slot N", and generated code inlines that load. So
this chunk is not two features with a shared week; it is one change to the
value model, with the caches as the place where getting it wrong is silent.

Before it, `delete` was `unsupported construct: delete (objects have no
dictionary mode yet)`, an object-literal accessor was `unsupported construct:
object literal getter or setter (accessor properties are not implemented)`,
and a class one was `unsupported construct: class getter or setter`. Two
blocked oracle cases (docs/0003) held the hand-derived expectations; both are
promoted here, and four new cases pin what they did not reach.

## Decision 1 — the first successful delete moves that object, and only that object, to a dictionary

A shape node records a key and implies its `slot_index` by its position in
the transition chain. Shapes are immortal, live in the non-moving arena, and
are **shared**: every object that added `a` then `b` points at the same node.
Removing `b` from the middle would renumber every node below it, for every
object that ever took that path, most of which are not the object being
deleted from. There is no local repair.

So `ObjectHeader::deleteProperty` gives the object a **private** `Shape`
whose `dict` points at a `Dictionary` — an entry vector, owned by that one
object, that something can be erased from the middle of. `src/runtime/
dictionary.{h,cpp}`. Each `DictEntry` carries exactly what a shape node
carried: the interned name, the slot, `enumerable`, and `accessor`.

Three details are load-bearing.

**The private shape keeps the same `root`.** A shape carries the prototype
(docs/0008 decision 1), and the prototype value lives in the root shape's
slot, which the GC already knows about. Copying the root pointer rather than
the value is what keeps a dictionary transition from introducing a new place
the collector has to be taught about.

**The entry's name is interned into the arena.** A dictionary entry outlives
every collection, so the string it holds must too. That is not automatic:
`o[k] = v` arrives with a freshly allocated *heap* string as its key, and
pointing an entry at that would leave a dangling name after the next GC.
`dictDefine` interns, exactly as the shape path always did.

**A freed slot returns to a free list, but only a data slot.** `o.k = v;
delete o.k;` in a loop must not grow the object's slot storage without bound.
An accessor's two slots are adjacent by construction, and a free list cannot
promise adjacency to the next accessor that asks, so a released pair is not
reused.

An object stays a dictionary once it becomes one. The alternative — noticing
that it has settled and migrating it back to a shape chain — is a real
optimization that real VMs do, and it is not here; see "what is not here".

The 1024-property threshold that docs/0004 named stays a **hard error**, not
a silent demotion to dictionary mode. Dictionary mode as built is a linear
scan over an entry vector: it is the right answer for an object that had
three properties and lost one, and the wrong answer for an object with a
thousand. Diagnosing that by name is honest; quietly making a 1000-key map
O(n) per lookup is the shape of problem that hides for a year.

## Decision 2 — `delete` is a reference operator, and an array element deleted becomes a hole

13.5.1.2 evaluates the operand and asks whether it produced a **Reference**.
That is the whole operator, and it is why the lowering dispatches on the AST
shape of the operand rather than on a value:

- `o.k` and `o[e]` are property references — `prop.delete` / `elem.delete`.
- Anything else is not a reference: the operand is evaluated **for its
  effects** and the result is `true`. `delete f()` calls `f`.
- `delete x` names a binding. 13.5.1.1 makes it an early SyntaxError in
  strict mode, and the sloppy reading is not one bronze could express anyway
  — it has no global object to delete from. Named error.
- `delete super.x` is always a ReferenceError (13.5.1.2 step 2). Named error.

The result is `true` in every case except a non-configurable property, and
bronze has no non-configurable properties (see "what is not here"), so
`delete` in bronze answers `false` only where node does too by accident of
what is not built: never. It answers `true` for a property that was never
there, and answering true **without touching the object** matters — it is
what keeps `delete o.missing` from demoting a record to a dictionary.

`delete a?.b` is legal and short-circuits, and it is the one place a
short-circuited optional chain does not produce `undefined`: 13.5.1.2 asks
whether the operand produced a Reference, and a chain that stopped early
produced none, so the answer is `true`. The chain machinery of docs/0018
decision 7 already collected the short-circuit edges as an n-way join; this
needed only a second thing for those edges to materialize, hence
`lowerChainJoin(..., ChainMiss::True, ...)`.

**An array index is different from a named property.** `delete a[1]` leaves a
**hole**: `length` is untouched (10.4.2.1 makes `length` a separate own
property that nothing but a write to it changes), the index stops being an
own property, and a read answers `undefined`. Holes were previously
unreachable in bronze — the only other way to make one is a sparse write,
which is a named hard error — so this decision creates a state the whole
array surface has to have an answer for.

The rule is that **reading a hole and asking whether a hole is there are
different questions**, and ECMA-262 already says which methods ask which:

- Defined over own keys, so a hole is skipped: `Object.keys`, `for-in`,
  object spread, `in`, and the `Array.prototype` methods specified with
  HasProperty — `forEach`, `map`, `filter`, `some`, `every`, `reduce`,
  `reduceRight`, `indexOf`, `lastIndexOf`, `slice`, `concat`.
- Defined over `Get`, so a hole *is* `undefined`: `find`, `findIndex`,
  `findLast`, `findLastIndex`, `includes`, `at`, `join`, the array iterator
  — and therefore `for-of` and array spread, which densify.

`indexOf(undefined)` is `-1` and `includes(undefined)` is `true` on the same
array. That is not an inconsistency to be smoothed over; it is the two
questions, and `array_holes.js` pins the pair on adjacent lines.

Of the producers, `map`, `slice` and `concat` copy the absence — `map`'s
output length is its input's by definition — while `filter` densifies,
because it emits nothing for an element it did not test. `reduce` with no
initial value seeds from the first element that is **present**, not from
index 0.

`console.log` prints a run of holes as one entry, `<1 empty item>` or
`<n empty items>`, which is node's format and the one docs/0013 pins.

## Decision 3 — an accessor runs with the receiver as `this`, and there are four ways to reach one

A getter is not a property of the object it was defined on; it is a property
of whatever the read went **through**. `Object.getPrototypeOf(c).diameter`'s
getter, reached as `c.diameter`, must see `c`. That is what makes an accessor
usable as a computed field on every instance of a class at once, and it is
6.2.5.5's `Receiver` parameter threaded all the way down.

Threading it is the entire cost, and the reason this is a decision rather
than a line of code: bronze has **four** paths that reach a property, each of
which had a natural implementation that passes the wrong `this`.

1. **The ordinary walk.** `getProp` gained a `receiver` parameter defaulting
   to null, meaning "the object itself". The proto walk passes it down
   unchanged, so an inherited accessor sees the instance.
2. **A function's statics.** A function's own properties live in a side
   object (docs/0012 decision 6), so the natural `this` for a `static get` is
   that box. It has to be the constructor. `rt_prop.cpp` passes the function
   value down explicitly, and `accessor_receivers.js` reads `this === Reg` to
   discriminate the two.
3. **`super.x`.** The lookup starts at the parent prototype but the receiver
   stays `this` (13.3.7.3). As a plain `prop.get` on the prototype — which is
   what `super.x` lowered to before accessors existed, correctly, because a
   method does not care — a base getter would have read the *prototype's*
   fields. New IL op `super.get` and ABI function `bronze_super_get`, whose
   only job is to carry the third operand.
4. **Object spread.** 7.3.25 copies with `Get`, so the getter runs once at
   spread time and the copy holds a **data property** with its value. That
   falls out of `copyProperty` already using `getProp`/`setProp` — but only
   because those are Get/Set semantics, which is the next decision.

A write reaches a setter through the same four paths plus one more question:
an assignment that finds no own property must walk the prototype chain
looking for an **inherited setter** before creating an own data property.
`setProp` does. The operations that must *not* — `method.def`, and the
property creation inside an object literal or a spread target — pass
`defineOwn=true`, which is DefineOwnProperty rather than Set. Without that
flag, `class D extends B` defining a method whose name the base defines as an
accessor would have *called the base's setter* instead of defining a method.

An undefined getter answers `undefined` (10.1.8.1) rather than the getter
function; an undefined setter is decision 6.

## Decision 4 — one property with two halves, and enumerability splits by where it was written

`{ get x() {}, set x(v) {} }` is **one** property. The shape node carries
`accessor` as part of the transition key and reserves **two adjacent slots**:
`slot` for the getter, `slot + 1` for the setter, either of which may be
`undefined`. `defineAccessor` looks for an existing accessor of the same name
on the same object first and fills the missing half in place, with no shape
change at all — which is what makes the object literal above have one key and
not two.

A data property and an accessor are mutually exclusive, so redefining one as
the other is a transition, not an update. Redefining a **data** property as an
accessor is supported, by moving the object to dictionary mode: the data slot
has to be abandoned and a pair allocated, which is exactly the middle-of-the-
chain problem decision 1 solves. The reverse — redefining an accessor as a
data property under `defineOwn` — is a named hard error, because the only
route to it today is `Object.defineProperty`, which does not exist.

**Enumerability differs by where the accessor was written, and that is the
spec, not a convenience.** An accessor in an object literal is enumerable
(10.1.9.2 via PropertyDefinitionEvaluation); one in a class body is not
(15.7.14 defines it exactly as it defines a method). So an object literal's
getter appears in `Object.keys` and is copied by spread; a class's getter
appears in neither, on any instance. This is the same rule that already kept
class methods out of `for-in` (docs/0018 decision 2), and it now has a second
carrier: `accessor.def` takes an `enumerable` operand, and the two lowerings
pass different constants.

`delete` of an accessor removes the **pair**. `console.log` of one names the
halves — `[Getter]`, `[Setter]`, `[Getter/Setter]` — and deliberately does
**not** run the getter: inspecting a value must not have effects, and the
inspect walk allocates nothing.

The parser enforces 15.4.1's arity as an **early** error: a getter that took
a parameter could never be given one, and a setter that took none would
silently discard every write. A computed accessor name is a named
unsupported-construct error rather than a silent misparse.

## Decision 5 — an inline cache entry always describes a data property in a shape-indexed slot

This is the decision the rest of the chunk exists to make safely, and it is
stated as an invariant on the entry rather than as a rule for each site,
because its consumers cannot all check the same things.

An `InlineCache` is `(Shape* cached_shape, uint32_t cached_slot, uint32_t
cached_depth)`. Generated code inlines the check for the common case
(docs/0010 decision 7) and can do exactly one thing on a hit: load a slot. It
cannot call a getter, and it cannot index a dictionary's entry vector. So:

- **An accessor is never written into an entry.** A cached hit that folded
  one into a load would return the getter function instead of running it.
- **A dictionary-mode object's shape is never written into an entry.** Its
  slots are not shape-indexed; the shape is a private box for a table.

Both fall out for free at the receiver, since a dictionary object gets a
private shape that no entry has ever seen, so the shape compare simply
misses. The IC layout, the ABI, and the inlined fast path in
`src/codegen-llvm/llvm_prop.cpp` needed **no change at all** — which is the
argument that the design is right, and is why this chunk did not have to
touch the generated-code contract.

The sharp case is the one that does not fall out: **a cached proto hit checks
the receiver's shape, and the receiver's shape does not change when its
prototype is deleted from.** The freed slot goes on the prototype's free list
and is handed to the next property added there, so a stale entry does not
return `undefined` — it returns another property's real value. Silent, and
wrong.

So a hit at `cached_depth > 0` is taken only after re-deriving the holder and
checking it is not a dictionary (`cachedProtoHolderIsStale`, in both
`object.cpp` and the `rt_prop.cpp` fast path). Filling obeys the same rule.
The holder is derived from the non-moving shape chain rather than cached, so
this costs a pointer walk that the depth field already required.

`inline_cache_after_delete.js` proves the guard is load-bearing, and the way
it does so is worth recording, because the first version of it proved
nothing. Every read in it goes through **one shared function**, on purpose:
two syntactically separate `q.b` expressions are two IC sites with two cold
caches, and a case written that way passes with the guard removed. Only a
site that is warmed and then re-entered can observe a stale entry. With the
guard disabled, the rewritten case returns a shadowed property's value where
`undefined` is correct.

`inline_cache_shape_changes.js` is the mixed-shape case: one property
expression, in a loop, over an array holding a plain object, a dictionary and
an object with a getter — with plain objects **first**, so the cache is warm
and monomorphic before the other two arrive, and with the dictionary and the
accessor each appearing twice consecutively, so a poisoned entry would be
re-hit rather than merely written.

There is one hole this decision does not close, and it predates the chunk:
see "the bug this chunk did not fix".

## Decision 6 — writing a getter-only property is a silent no-op

10.1.9.2 returns `false` when a property has no setter, and a non-strict
assignment discards that result. Strict mode makes it a TypeError, and bronze
has no strict mode (`cases/blocked/strict_mode.js`).

The three readings available were: silent no-op (sloppy mode, and what the
pinned `accessor_properties.expected` requires), a named hard error, or
creating an own data property that shadows the accessor. The third is simply
wrong. The second is what CLAUDE.md's "hard errors over silent fallbacks"
would normally choose — but that rule is about constructs bronze has not
**built**, not about behaviour ECMA-262 defines as silent, and `square.area =
100` doing nothing is the answer every JavaScript engine gives.

This decision expected `throw` to turn it into a TypeError. docs/0020 landed
`throw` and left the no-op alone, because the reading was half wrong: 13.15.2
PutValue step 6.d throws only when the REFERENCE IS STRICT, so raising here
unconditionally would be wrong for the sloppy code that is bronze's only mode
and would contradict `accessor_properties.expected`. The missing piece is the
Directive Prologue of 11.2.2, not the mechanism.

Symmetrically, a setter-only property reads as `undefined`, not as the setter
function.

## Deliberate divergences from node

- **`delete` never answers `false`.** Every property in bronze is
  configurable, so there is nothing for it to refuse. A program that depends
  on `delete` failing is depending on `Object.defineProperty` or
  `Object.freeze`, neither of which exists — both are named errors, so the
  divergence cannot be reached silently.
- **Dictionary mode is permanent and linear.** An object that lost a property
  never returns to a shape chain, and its lookups are a scan. Correct, and
  slower than it needs to be for an object that is deleted from once and then
  read from a million times.
- **A getter's `this` in a detached call is not modelled.** `const g =
  Object.getOwnPropertyDescriptor(o, 'x').get` cannot be written, so the
  receiver is always the one the read went through.

## Named diagnostics this chunk adds

Parse:

- ``unsupported construct: a computed getter name (`get [e]() {}`)``
- ``unsupported construct: a computed setter name (`set [e]() {}`)``
- `expected a getter name: an identifier or a string literal`
- `expected a setter name: an identifier or a string literal`
- `a getter must take exactly no parameters`
- `a setter must take exactly one parameter`
- `a setter's parameter may not be a rest parameter`

Lower:

- ``unsupported construct: `delete x` deletes a binding, which is a
  SyntaxError in strict mode (delete removes a property: write `delete
  o.x`)``
- ``` `delete super.x` is always a ReferenceError (ECMA-262 13.5.1.2) ```
- `internal: an accessor property with no function`

Runtime:

- `deleting property 'k' of null` / `... of undefined`
- `deleting a computed property of null or undefined`
- `redefining an accessor property as a data property is unsupported`
- `property delete with an unregistered key index`
- `internal: a property's getter slot holds something that is not a function`
  (and `setter`)
- `internal: a shape transition attempted on a dictionary-mode object`
- `internal: a dictionary definition on an object that is not in dictionary
  mode`
- `internal: an accessor defined on an object with no shape`

Retired: `unsupported construct: delete (objects have no dictionary mode
yet)`, `unsupported construct: class getter or setter`, and `unsupported
construct: object literal getter or setter (accessor properties are not
implemented)`. The two parser tests that pinned the absence of accessors are
replaced by positive assertions plus the new arity and computed-name errors.

## The bug this chunk did not fix

**A cached proto hit at depth 2 or more is not invalidated when an
intermediate prototype gains the property.** Adding `p` to the middle link of
a three-link chain changes only that link's shape; the receiver's shape word
is untouched, so a warm entry still hits, still walks `depth` links, and
still reads the property it shadowed. The same read at a cold site gives the
right answer, so a program can print two different values for one property.

It predates this chunk — it needs neither `delete` nor an accessor, only a
property added to a prototype after a site went warm — and the delete half of
the same question is closed by decision 5, because a delete moves the holder
to dictionary mode. It is only the **add** that gets through, since an add is
precisely the operation the shape chain makes free.

It is not fixed here because the fix does not fit: something must be able to
say "an object between this receiver and this holder has changed", and an
entry has 16 bytes with no room for it. Both standard answers — a validity
cell the entry points at, or a global epoch every entry records — widen
`InlineCache`, which widens the stride that `llvm_prop.cpp` inlines. That is
a change to the generated-code contract, so it is its own chunk.
`cases/blocked/proto_chain_invalidation.js` holds the correct expectations
and is the only case in that directory blocked on a bug rather than on a
missing feature.

## Bugs this chunk uncovered in existing code

1. **`bronze_for_in_keys` read an unrooted receiver after allocating.** The
   hole check was asked of the raw incoming `Value` *after* the result array
   had been allocated, so under `BRONZE_GC_STRESS=1` it read dead from-space
   and every hole was enumerated. Caught only by `oracle-gc-stress`, which is
   the third time in five chunks.

2. **Object spread of an array copied holes as `undefined`.** The array
   branch walked `0..length` rather than the own keys, so `{ ...a }` after a
   delete had a `'1'` key that `Object.keys(a)`, `for-in` and `in` all agreed
   was not there. Introduced by decision 2 and fixed with it.

3. **`Array.prototype` was hole-blind.** Ten methods visited holes as
   `undefined`; `[1, , 3].indexOf(undefined)` was `1`. Also created by
   decision 2, and fixed by it — see the method split there.

## What is not here

- **`Object.defineProperty`, `getOwnPropertyDescriptor`, `freeze`.** They are
  the natural successor and they need two more attribute bits in the shape
  transition key, a real `[[DefineOwnProperty]]`, a per-object extensible
  bit, and a descriptor-object round trip.
  `cases/blocked/property_descriptors.js` holds the argument and the
  expectations.
- **`writable` and `configurable`.** A bronze property has exactly two
  attributes: enumerable, and accessor-or-data. See above.
- **Migrating a settled dictionary back to a shape chain.**
- **Symbol-keyed properties**, and therefore `Symbol.iterator` — the other
  thing standing between bronze and a real `Map` (`cases/blocked/
  map_and_set.js`).
- **`delete` reporting failure**, which needs `configurable`, and the
  strict-mode TypeError for a getter-only write, which needs `throw`.

## Files this chunk added

`src/runtime/dictionary.{h,cpp}` — decision 1's entry table and the
transition into it. `src/runtime/accessor.{h,cpp}` — decision 3's two calls
and decision 4's definition path. `src/runtime/rt_delete.cpp` — the two ABI
entry points, kept out of `rt_prop.cpp` because deleting asks a different
question of the same objects. The four seams were chosen before the code was
written, on the ground that `object.cpp` and `rt_prop.cpp` were the two files
most likely to cross 1000 lines otherwise.
