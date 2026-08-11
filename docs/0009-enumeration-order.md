# 0009 — Enumeration order and the dictionary boundary

Status: designed and implemented 2026-08-10. Implements the last two
unbuilt bullets of docs/0004 decision 2: spec'd enumeration order, and the
dictionary-mode transition point.

Enumeration order is a **correctness surface**, not a nicety. JS pins it
exactly, real code depends on it (serialization, diffing, three.js's
uniform and attribute maps), and a compiler that gets it wrong produces
programs that differ from node in output rather than in speed. Because it
prints, the oracle can hold it byte-for-byte.

## Decision 1 — order is recovered from the shape, not stored

An object's own string keys, in insertion order, are already recorded: the
shape transition chain is a linked list from the newest property back to
the root, one node per property, in the order they were added. Walking
`parent` to the root and reversing yields insertion order exactly.

So no per-object key vector, no side table, nothing to keep in sync — and
objects sharing a shape share the answer. Storing an insertion-order list
per object would cost a pointer and an allocation on every object for
something the shape already knows.

Spec order for own keys is:

1. **integer-like keys** (canonical array indices) in ascending *numeric*
   order — `"2"` before `"10"`, which string order gets backwards;
2. **every other string key** in insertion order.

Integer-like means the canonical decimal form round-trips: `"0"`, `"7"`,
`"42"` qualify; `"01"`, `"1.0"`, `"-1"`, `" 1"`, and anything past 2^32-2
do not, and are ordinary string keys.

## Decision 2 — `Object.keys` is a compile-time-known builtin

bronze has no global object, so `Object` cannot be a variable lookup.
`Object.keys(o)` is recognized in lowering the same way `console.log` is,
and becomes a single IL instruction. Any *other* member of `Object`
(`assign`, `create`, `getPrototypeOf`, …) is a hard error naming what was
asked for, rather than an "undefined variable: Object" that names nothing.

`for-in` is the other spec surface for this order. It stays a named hard
error here: it needs an iteration protocol and a live-mutation rule
(properties added during the loop), which is its own piece of work.
`Object.keys` snapshots, so it needs neither, and it pins the order that
`for-in` will later reuse.

`Object.keys` on an array yields its indices as strings, which is what
makes the integer-first rule observable on the container that has the most
of them. On anything else — a number, a string, a function — it is a hard
error rather than a silent empty array, because a silent `[]` is exactly
the shape of bug that hides for a long time.

## Decision 3 — the dictionary boundary is a named error, not a slow path

0004 accepted dictionary mode from the start and required that reaching it
be a named hard error until it lands, so the escape hatch never becomes a
silent-fallback temptation. Two things reach it:

- **`delete`.** The lexer learns the keyword purely so the construct can
  be *named*: before this, `delete o.a` was
  `expected ';' after expression statement, got 'o'`, which names nothing
  and reads like a parser bug. Now it is
  `unsupported construct: delete (objects have no dictionary mode yet)`.

- **Property-count churn.** An object accumulating a very large number of
  own properties is being used as a map, and the transition tree is the
  wrong structure for it: every property costs a shape node forever, and
  the chain walk that decision 1 relies on becomes linear in a big number.
  Past a threshold, that is diagnosed rather than silently paid for.

The threshold is **1024 own properties on one object**, deliberately far
above any record-like or class-like object and far below where the
transition tree's costs stop being noise. It is a boundary marker, not a
tuning knob: its job is to make the day bronze meets a real dictionary a
loud one.

Note what is *not* on the list: many *distinct shapes* branching from one
node. Since 0008 every plain `{}` shares one root shape, so that root's
transition count is the number of distinct first-property names in the
whole program — a large number in normal code, and no evidence at all
that any one object is a dictionary.

## What shipped, and what is deliberately not here

`enumeration_order` is the pinned oracle case, run under `oracle-gc-stress`
too. It covers integer-before-string ordering with `"10"` and `"2"` present
so string order would visibly disagree; insertion order surviving
reassignment; keys that only *look* like integers (`"01"`, `"-1"`, `"1.0"`,
`2^32-1`) staying string keys; object-literal source order; own keys only,
with a prototype property that must not appear; the empty object; and an
array's indices.

Deleting the numeric sort fails that case, which is the point of choosing
`"10"` and `"2"` rather than two single-digit keys.

Named hard errors:

- `unsupported construct: delete (objects have no dictionary mode yet)`
- `object has too many properties for the shape transition tree;
  dictionary mode is not implemented` (decision 3's threshold)
- `unsupported builtin: Object.<name>` for any member but `keys`
- `Object.keys on a value that is not an object`
- `Object.keys is only supported on plain objects and arrays`
- `unsupported construct: for-in loop` (unchanged; decision 2)

Not here, and named as such:

- **`for-in`**, which needs an iteration protocol and a rule for
  properties added or deleted mid-loop.
- **Dictionary mode itself.** The boundary is marked and diagnosed; the
  hash-backed representation behind it is unbuilt.
- **`console.log` of an array** still prints `[object]`, which is why the
  case walks the result instead of printing it. Pinning a container format
  is its own decision (node prints `[ 'a', 'b' ]`), and inventing one here
  would have quietly pinned it in an `.expected` file as a side effect.

  **Amended 2026-08-11.** `[object]` was the *fallthrough* branch of
  `bronze_print_value`, not a branch guarded on "is a container" — so it
  also swallowed `null`, which carries its own tag (`0xFFF5`) and therefore
  never satisfied the `undefined` check above it. `console.log(null)`
  printed `[object]` where node prints `null`. The suite could not catch it:
  `null` appeared in it only as an operand (`null ?? 5`), never as a printed
  value. Every tag is now handled explicitly — `null` prints, the reserved
  Int32 tag prints as the number it is, and the internal Hole and the
  unimplemented Symbol are hard errors rather than text, so a sentinel that
  escapes to `console.log` is loud instead of disguised as a container. The
  container deferral above is unchanged and is now the *only* thing
  `[object]` means. Pinned by the `print_primitives` oracle case, which
  reaches `null` through a binding, a branch join, a property slot, an array
  element and a function return; deleting the branch fails it on line 1.
- **Symbol keys**, which have their own place in spec order — bronze has no
  symbols.
