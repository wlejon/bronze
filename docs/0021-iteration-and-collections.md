# 0021 — Iteration and collections: a real iterator, keys that are values, and the two attributes a property was missing

Status: implemented. Part of phase 4 of docs/0001.

Three things that look like three chunks and are one. `Map` and `Set` cannot
be written without an iterator protocol, because `new Set(iterable)`,
`[...set]` and `for (const [k, v] of map)` are all the protocol and none of
them is a length walk. The iterator protocol cannot be written without an
answer to `Symbol.iterator`, because that is the name the protocol is defined
in terms of. And property descriptors are here because they are the other
thing docs/0019 left a hole for, and because `Object.freeze` is the case that
tells us whether the inline caches survived the last two chunks.

docs/0012 decision 3 said what this chunk had to undo:

> `for-of` is an INDEX WALK. `iter.length` then `iter.at` per element, over
> an array, a string or a typed array; anything else is a named error. A real
> iterator protocol needs `Symbol.iterator`, and bronze has no symbols.

and docs/0019 said the same about the property model:

> A bronze property has two attributes, `enumerable` and `accessor`. There is
> no `writable` and no `configurable`, which is why `delete` in bronze can
> never answer `false`.

Both statements are retired below. JSON is *not* in this chunk; it is
self-contained, it needs a second parser of its own, and
`cases/blocked/json.js` says why.

## Decision 1 — `Symbol.iterator` is the string `"@@iterator"`, and there is no symbol primitive

The value model has room for symbols: tag `0xFFF8` is reserved and unused
(docs/0004). The value is not the cost. The cost is that a property KEY in
bronze is an arena-interned `StringHeader*` compared by **content**
(docs/0009) — that content comparison is exactly what makes two objects
written with the same property names share a shape, and it is open-coded in
the inline-cache fast path generated code emits (docs/0010 decision 7). A
symbol key is the opposite rule: compared by **identity**, so two symbols with
the same description are two different keys. Landing symbols means either a
parallel key type threaded through shapes, dictionaries, enumeration, the
caches and `Object.keys`, or a second matching rule that applies to symbol
transitions only. Both are value-model work, and neither is a prerequisite for
iterating a user object.

So: `Symbol` is a function object whose only property is `iterator`, and
`Symbol.iterator` is the string `"@@iterator"`. `Symbol()` itself is a named
hard error rather than a stub, per docs/0011 decision 1.

What makes this more than a hack is the compensating rule, and it is one line
in `ObjectHeader::setProp`: **an own property whose name begins with `@@` is
created non-enumerable.** That is the one property of a symbol key the
protocol actually depends on — an object with an `@@iterator` method must
still report the keys the program wrote, to `Object.keys`, to `for-in`, to
`Object.entries` and to spread. The rule lives in the runtime rather than in
lowering because one of the four ways to reach it is `o[k] = v` with a
computed key, where the name is not known until run time.

The divergences are real and are pinned as divergences rather than hidden:
`typeof Symbol.iterator` answers `"string"`; a program *can* make an object
iterable by writing the literal key `"@@iterator"`; and a program that uses
`@@`-prefixed keys of its own gets non-enumerable properties it did not ask
for. `cases/blocked/symbols.js` holds the expectations for the real thing and
names the value-model work as the blocker. `cases/iterator_protocol.js` pins
the divergences from the other side, so the day symbols land, that case fails
and has to be updated deliberately.

## Decision 2 — one iteration RECORD, with fast kinds and a protocol kind

`iter.length` / `iter.at` / `iter.advance` are gone. In their place is
`iter.open` producing an **iteration record**, and `iter.step` / `iter.value`
/ `iter.close` over it. The record is a heap object (`flags == 7`) holding the
target, the `next` function, the current value, a cursor, a kind and a done
flag.

The kind is decided **once**, at open time, and it is the whole point. An
array, a string, a typed array, a Map and a Set take a FAST kind: the cursor
is stepped by the runtime, and there is no iterator object, no `{ value, done
}` result object and no call into user code per element. That is what the
index walk bought and what a naive "everything is the protocol" rewrite would
have given back — the protocol allocates a result object per element by
construction. Only a value that is none of those takes the `Protocol` kind and
runs 7.4.2 GetIterator for real.

`iter.step` returns an `i1` and `iter.value` reads the record, rather than one
instruction returning a pair, because the IL has no tuples and because the
loop header wants the boolean for a `br` and the body wants the value — two
different blocks (docs/0005).

A Map's default iterator is the one fast kind that allocates: 24.1.3.12 says
it yields `[key, value]` pairs, so the pair array is built per step. A Set's
is not, since it yields the key itself.

## Decision 3 — IteratorClose is a cleanup frame, not a finally

docs/0020 built `finallyStack_` so a `break` out of a `try` runs the
intervening `finally` blocks. An abandoned `for-of` has to run something very
similar — 7.4.9 IteratorClose — and the temptation is to reuse the same stack
by synthesizing a `finally`. That is wrong in two observable ways, and
`cases/iterator_close.js` pins both:

- **Ordering.** A `finally` *inside* the loop body runs BEFORE the close,
  because it is part of evaluating the body and the loop acts on the body's
  completion afterwards. A `finally` *outside* the loop runs AFTER it. A
  synthesized finally has one position and cannot be both.
- **Which jumps cross it.** A `break` leaves the loop and must close; a
  `continue` stays in it and must not. A `finally` does not have that
  distinction, because nothing continues *through* a `try`.

So `finallyStack_` became `cleanupStack_` with two kinds, and `JumpTarget`
records **two** cleanup depths — the depth at the loop's entry, which a
`break` unwinds to, and the depth inside its body, which a `continue`
unwinds to. The throw path is a per-loop handler block containing `exc.take`,
`iter.close %rec, suppress`, `throw`; it reads no binding, so it needs no
block parameters, which is why it can be created once per loop rather than per
exit.

`suppress` is 7.4.9 step 6, and it is a flag on the instruction rather than a
runtime decision: when a throw is already on its way out the caller has
already taken the pending value with `exc.take`, so the runtime cannot see for
itself that an error from `return` should be discarded.

Array destructuring closes too (8.6.2 step 5) — and does not when there is a
rest element, because the rest drained the iterator and there is nothing left
to abandon.

## Decision 4 — a Map is a table of VALUES, with an epoch on its index

A `Map` is not an object with properties, and modelling it as one would have
been the easy wrong answer: property names are interned strings compared by
content, and a Map key is any value compared by SameValueZero (7.2.10), where
`NaN` finds itself and `+0` and `-0` are one key.

The layout is an entry vector (two `Value`s per slot, key then value, a `Hole`
key marking a tombstone) plus an open-addressed bucket index. Insertion order
IS the entry order, so iteration is a scan and `set` on an existing key writes
in place without moving it; a delete leaves a tombstone and the bucket keeps
pointing at it, so probes run past rather than needing a chain repair. Growth
is sized from the LIVE count rather than the old capacity, which makes
`m.set(k, v); m.delete(k)` in a loop compact instead of grow.

Two GC decisions, and they are the ones worth the words:

- The whole payload is `Value`s, so the collector's generic scan forwards the
  keys and values with no Map-specific code. The bucket index is *not* — it is
  `Tag::RawBytes`, precisely so the collector does not read `uint32` bucket
  numbers as `Value`s.
- **An object key can only be hashed by its address, and a semispace collector
  changes addresses.** So the index records `Heap::collection_count()` when it
  was built and is rebuilt lazily when that count has moved on. A stale bucket
  does not crash; it answers "not found" for a key the map still holds, which
  is a silent wrong answer. `tests/runtime/map_test.cpp` proves the index
  against a linear scan over 300 mixed-kind keys, before and after collections
  and across a compaction, and `cases/map_gc_keys.js` does the same from the
  outside under `oracle-gc-stress`.

`Map` and `Set` are one header and one implementation, distinguished by
`HeapObjectHeader::flags` (5 and 6) and by what the iterator yields. The
methods that only one of them has (`get`, `add`) are named errors on the
other.

## Decision 5 — `writable`, `configurable` and `extensible` live in the dictionary

A property descriptor adds two attributes per property and one per object. The
obvious place is the shape transition key, which is `(name, enumerable,
accessor)` today. That is the expensive place: it doubles the ways two objects
that "have the same properties" fail to share a shape, and a `writable: false`
added late forks the transition tree for every object that had reached that
point.

So they do not go there. `writable` and `configurable` are fields of
`DictEntry`, `extensible` is a field of `Dictionary`, and **any property with a
non-default attribute moves its object to dictionary mode** — which docs/0019
already built for `delete`, and which already gives an object a private shape
that no inline cache has ever seen or ever can match. The shape transition key
does not grow, `InlineCache` does not grow, `bronze_abi.h` is untouched, and
`llvm_prop.cpp`'s open-coded fast path did not change a line.

That is also the answer to the third thing a cache entry could be wrong about.
`inline_cache_shape_changes.js` pins the two from docs/0019 (a dictionary's
slots are not shape-indexed; an accessor is not a slot). Non-writable would
have been the third, and it is not, because a non-writable property implies a
private shape implies a permanent cache miss.
`cases/frozen_inline_cache.js` proves it from the outside, warming each site
with a plain receiver first so the entry is actually filled before the frozen
one arrives.

The consequence docs/0019 flagged is retired: `delete` can answer `false`
now, and a write can be silently discarded for a reason other than a missing
setter.

`defineProperty` always goes through dictionary mode, even for a descriptor
whose attributes are all the defaults. The alternative — deciding per
descriptor whether the shape path would do — makes `defineProperty` and
assignment two paths that must agree about the enumerable/accessor cases as
well, and there is no evidence anyone calls `defineProperty` in a hot loop.

## Decision 6 — `Object` is a namespace object, with `keys` kept as a fast path

`Object.keys` used to be recognised at the CALL SITE: lowering matched the
member expression and emitted an IL instruction, and every other member of
`Object` was a compile error. That does not extend — `Object.assign` passed to
a higher-order function, or `const O = Object`, are both ordinary JavaScript.

`Object` is now a real object with real function properties, resolved by
`bronze_global_get` like `Math`. `Object.keys` keeps its IL instruction as a
**fast path** — matched only when the call is literally `Object.keys(x)` with
one non-spread argument — and both paths call the same C++ function, so the
two can never answer differently. Everything else falls through to an ordinary
call.

## Decision 7 — `Number` is the namespace, and the wrapper methods are not here

`Number.isInteger` / `isNaN` / `isFinite` / `isSafeInteger`, `parseInt`,
`parseFloat` and the eight constants are statics on a namespace object and
land the same way `Math` did.

`Number.prototype.toFixed` is deliberately not here, and it is not a matter of
plumbing. 21.1.3.3 is defined on the exact mathematical value of the double,
which is why `(1.005).toFixed(2)` is `"1.00"`; implementing it as a `printf`
or a `to_chars` round would be a silent wrong answer in exactly the code that
reaches for it. `cases/blocked/number_methods.js` holds the hand-derived
expectations, `toExponential`, `toPrecision` and `toString(radix)` alongside.

## Decision 8 — a reserved word is a property name

`Map.prototype.delete` and an iterator's `return` method are both spelled with
a reserved word, and both are ordinary IdentifierNames (12.7.1): after a `.`,
after a `?.`, as an object-literal key, and as a class member name. bronze's
lexer produces keyword tokens for all of them and the parser demanded
`Identifier`, so `m.delete(k)` and `{ return: f }` were parse errors — which
is not a hole anyone would have found without needing to write an iterator.

The positions that are NOT property names keep the old rule: `{ x }` and
`{ x = 1 }` want an IdentifierReference, because they evaluate the name, and
no binding is spelled `return`.

## What this chunk stopped short of

- **JSON.** `cases/blocked/json.js`. Self-contained, and the natural split:
  `stringify` is a pinned byte format that is deliberately *not*
  `console.log`'s (docs/0013), and `parse` is a second parser with a grammar
  that is not JavaScript's, so it must be its own module under the isolation
  rule rather than a borrow from `src/parse`.
- **Symbols.** `cases/blocked/symbols.js`, per decision 1.
- **The Number wrapper methods.** `cases/blocked/number_methods.js`, per
  decision 7.
- **`Object.create`, `getPrototypeOf`, `setPrototypeOf`, `seal`,
  `preventExtensions`, `getOwnPropertyNames`, `defineProperties`.** Real
  members bronze has not built, so they are named errors from
  `kObjectUnimplemented` rather than absent. `seal` in particular is
  `freeze` minus the writable half and would be three lines; it is left out
  because nothing pins it, and an unpinned builtin is how docs/0000's
  "plausible but wrong" bugs got in.
- **`WeakMap` / `WeakSet`.** They need the collector to drop an entry whose
  key died, which is a GC decision, not a table one.

## Errors this chunk added (all `TypeError`, docs/0020's cell)

- `<kind> is not iterable`
- `the result of Symbol.iterator is not an object`
- `the iterator has no \`next\` method`
- `the iterator result is not an object`
- `Cannot redefine property: <k>` / `Cannot define property, object is not extensible`
- `Invalid property descriptor. Cannot both specify accessors and a value or writable attribute`
- `Property description must be an object`
- `Object.<m> called on a value that is not an object` /
  `Object.assign target must be an object`
- `Iterator value is not an entry object`
- `Method <m> called on an incompatible receiver`
- `Map.prototype.forEach needs a function argument`

## Files this chunk added, and the seams it cut

The 1000-line rule was never close, because the seams were cut before the code
was written rather than after:

- `src/runtime/map.{h,cpp}` — the TABLE, and nothing else: SameValueZero, the
  hash, the entry vector, the index and its epoch. It has no idea what a
  JavaScript `Map` method is, which is what lets `tests/runtime/map_test.cpp`
  prove it against a linear scan without going through the ABI.
- `src/runtime/builtin_map.cpp` — the JavaScript surface over that table: the
  methods, the iterator objects `keys()`/`values()`/`entries()` return, and
  the constructors. The seam is "data structure" against "language surface".
- `src/runtime/iterator.{h,cpp}` — the record, the fast kinds, the protocol,
  the five ABI entry points, and `Symbol`. `Symbol` is here rather than in a
  file of its own because decision 1 makes it a one-property object whose only
  reason to exist is `Symbol.iterator`; separating them would name a seam that
  is not there. Replaces `src/runtime/rt_iter.cpp`.
- `src/runtime/builtin_object.cpp` and `src/runtime/builtin_number.cpp` — one
  namespace each, following `builtin_array.cpp` and `builtin_string.cpp`. The
  descriptor operations are in the `Object` file rather than in
  `dictionary.cpp` because `dictionary.cpp` owns the *storage* of an attribute
  and `builtin_object.cpp` owns the *spec algorithm* that decides one.
- `src/lower/lower_iter_loop.cpp` — rewritten around decision 3; it was
  `lowerIndexWalkLoop`, which is no longer what it does.
