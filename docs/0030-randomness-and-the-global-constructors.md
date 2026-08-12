# 0030 — Randomness, and `Array` / `String` / `Boolean` as global objects

Status: implemented.

The goal since docs/0001 has been to compile and run a real three.js
application headlessly. After docs/0029 that application compiled and linked —
29 modules of unmodified three.js, a 1.9 MB native executable, zero errors —
and stopped at run time on one line:

```
Hard runtime error: unsupported: Math.random is not implemented
```

Probing past each blocker in turn showed the whole remaining chain was three
things: `Math.random`, `Array` as a value, and `String` as a value. This
document is those three and the decisions they forced.

## Decision 1 — a compiled program gets REAL randomness, seeded from the OS

docs/0011 decision 3 put `Math.random` on the unimplemented list on purpose and
deferred the seeding question. This is the answer: **the generator is seeded
once per process from a non-deterministic OS source, so two runs of the same
compiled program produce different sequences.**

It looks like a violation of the house determinism rule and it is not. The rule
is docs/0001 decision 10, and what it constrains is **bronze's own output**: IL
dumps, diagnostics, float formatting, enumeration order, anything the compiler
emits or a test compares. It has never constrained the behaviour of the
programs bronze compiles, and it could not — a compiled program reads files,
takes arguments and will eventually take a clock.

The alternative was a fixed seed, and it is worse than it sounds:

- ECMA-262 21.3.2.27 asks for an implementation-dependent pseudo-random
  sequence "with approximately uniform distribution". A fixed seed satisfies
  the letter of that and defeats its purpose.
- Every compiled program would produce the identical "random" sequence on every
  run. Two processes shuffling a deck, picking a port, or generating a UUID
  would agree — silently. That is the exact shape docs/0000 calls a plausible
  wrong answer: it looks random, it passes a spot check, and it is not.
  `generateUUID` in `Object3D` and `BufferGeometry`, the call that blocked the
  application, is precisely a case where colliding output is a bug that surfaces
  far from its cause.
- The apparent conflict with the oracle suite is not one. docs/0003's harness
  already greps every case — in `cases/` and in `cases/blocked/` — and fails it
  if the source contains `Math.random`, alongside the same check for `Date`.
  **Verified: the grep is a literal `code.find("Math.random")` on the case
  source, so a case cannot call it and no pinned expectation can depend on it.**
  Nothing had to be weakened for this decision, because nothing was resting on
  the absence.

What that grep does NOT catch is an indirect spelling (`Math["ran" + "dom"]()`).
That is a deliberate non-goal: the check is a guard rail for the person writing
a case, not a sandbox, and the same is true of its `Date` half.

**The generator is xoshiro256++**, seeded through SplitMix64 from
`std::random_device` mixed with a high-resolution clock reading and a stack
address. `std::random_device` is the OS entropy source on every target
(RtlGenRandom on Windows, getrandom on Linux); the clock and the address are
belt and braces for a platform whose `random_device` is a deterministic
fallback, so such a platform still differs run to run rather than silently
repeating one sequence forever. SplitMix64 is the companion the xoshiro authors
specify, and it is there for a reason: it turns any 64-bit value into a
full-entropy stream, so a seed source that returns correlated or mostly-zero
words cannot leave the state near all-zeros, which is the one state xoshiro
escapes only slowly.

**It is deliberately not `rand()`**, and the reasons compound:

- MSVC's `RAND_MAX` is 32767. `rand()` yields fifteen bits per call, so a
  double built from it lands on a lattice of 32768 points instead of filling
  [0, 1).
- It is a Lehmer LCG whose low-order bits have a short period, so the cheap
  fixes (calling it three times and shifting) inherit the defect.
- Its state is a single process-global shared with any C library code, so a
  third party calling `srand` silently changes a JS program's sequence.
- `srand` takes 32 bits, which is fewer distinct streams than a long-running
  program can exhaust.

`Math.random` returns the top 53 bits — a double's whole mantissa — divided by
2^53. That is every representable double in [0, 1) with equal spacing, and it
cannot round up to exactly 1.0, which is the off-by-one that breaks
`a[Math.floor(r * a.length)]` once in a very long while.

Pinned by `tests/runtime/math_random_test.cpp` rather than by an oracle case,
because an oracle case is forbidden from mentioning it. The test draws 20000
values and checks the three things a broken implementation gets wrong: the range
(a shift by the wrong width puts values at or above 1), the movement (a
generator that is never stepped repeats), and the distribution (a generator with
a constant high bit still passes a naive range check).

## Decision 2 — `Array` is a real global object, through docs/0029's mechanism

`Array.isArray` is called in `BufferGeometry.setIndex`, `Object3D.toJSON`,
`Line`, `ShapeGeometry` and a dozen other places, and a bare `Array` did not
resolve at all: the name fell off lowering's resolution ladder and became
docs/0027's unresolved-name warning plus a runtime `ReferenceError`.

**The mechanism is docs/0029 decision 2's, inherited unchanged**, which is what
that decision was written to be:

1. `Lowerer::isProvidedGlobal` gains the names. The list stays closed, and the
   check stays the LAST resort in identifier lowering, so `const Array = ...`
   still shadows it with no special case.
2. `bronze_global_get` resolves the name through `rtGlobalConstructor`, which
   hands back `bronze_function_singleton(code, 0)` — **interned on the code
   pointer**, so every mention of `Array` in a program is one object and the
   global cache's entries are already a GC root source.
3. **One distinct C function per constructor.** That is load-bearing and not
   style: two constructors sharing a body with a kind parameter would intern to
   ONE object, and `Array === String` would be true.
4. `[].constructor` asks the same function, so `arr.constructor === Array` holds
   — the 10.2.5 back-pointer, reached the only way a header type with no shape
   can reach it: as a branch in the property path.
5. **Arity 0**, and here it decides an answer rather than a cost.
   `FunctionHeader::arity` is the count a short call is PADDED to, so `new
   Array(3)` under any fixed arity would arrive as `(3, undefined)`, take
   23.1.1.1's element-list branch, and produce `[3, undefined]` where the
   language says three holes.

What is built: `Array.isArray` (23.1.2.2), `Array.of` (23.1.2.3), `Array.from`
(23.1.2.1) over both the iterator path and the array-like path, `Array(...)` and
`new Array(...)` in both of 23.1.1.1's readings, and `constructor`. Checked
against what three.js actually calls first: `isArray`, `from`, `new Array(n)`,
and `Array.prototype.slice.call` — which is the one below.

Two corners are worth naming because each is a plausible wrong answer:

- **`new Array(3)` is three HOLES**, not three undefineds. 23.1.1.1 step 3.d
  writes only `length`, so `0 in new Array(3)` is false, `forEach` visits
  nothing, and console.log prints `[ <3 empty items> ]` (docs/0019 decision 2).
  A dense run of `undefined` would look right in every printout and be wrong in
  every method defined over HasProperty.
- **One numeric argument is a length and everything else is an element list.**
  `new Array("3")` has length 1. A length that is not a uint32 — negative,
  fractional, NaN, 2^32 — is 23.1.1.1 step 3.b's `RangeError: Invalid array
  length`, and a length the specification allows but this heap cannot hold is a
  second, different `RangeError` naming the heap, raised BEFORE the allocation
  so `std::bad_alloc` never unwinds out of a helper generated code called. That
  is docs/0029 decision 1's rule for a byte store, applied to a dense one.

## Decision 3 — `Array.prototype` is a named error, not the empty object

docs/0029 recorded a pre-existing hole: the function branch of the property path
answers `prototype` from the `FunctionHeader` before it consults any
unimplemented-member table, so `Float32Array.prototype` and `Map.prototype` read
as an empty object — silently. Installing a method on one would be found by
nothing.

`Array.prototype` must not land in it, because `Array.prototype.slice.call(...)`
is written twice in three.js and `Object.prototype` is already a named error in
`cases/blocked/object_intrinsic_prototypes`. Two answers to the same question
would be worse than either.

**So the global constructors' member hook runs BEFORE the `prototype` slot**,
and `prototype` is on each of their unimplemented lists. `Array.prototype`,
`String.prototype` and `Boolean.prototype` are `unsupported: X.prototype is not
implemented`. The hook diagnoses and then falls THROUGH for every other name, so
`Array.call` keeps the `Function.prototype` diagnosis it already had rather than
becoming a silent `undefined` — a hook that swallowed every miss would have
traded one hole for another.

This does not fix the typed-array and `Map` cases. They are a different code
path with pinned behaviour of their own, and widening the fix into them is a
change to what docs/0029 and docs/0021 recorded, not a change to this file.
The general repair is the value-model chunk `object_intrinsic_prototypes`
describes: real intrinsic prototype objects, every root shape pointing at the
right one, and the property path finding methods THROUGH them instead of beside
them.

## Decision 4 — `String` and `Boolean` are CONVERSIONS, and `Function` is not here

`String(x)` (22.1.1.1) and `Boolean(x)` (20.3.1.1) land through the same
mechanism, with `String.fromCharCode` (22.1.2.1) and the `constructor`
back-pointer on a primitive string. `String.prototype` as a real object with a
method surface is explicitly NOT here — that is the value-model chunk above, and
`"abc"[0]`, `"abc".length` and every `String.prototype` method keep the property
path they have.

Called with `new`, 22.1.1.1 and 20.3.1.1 build a wrapper OBJECT, and bronze has
none. A native constructor cannot see NewTarget through the uniform calling
convention (docs/0004) — the same reason docs/0029 recorded the mirror-image
divergence for the typed arrays, where calling one WITHOUT `new` builds the
object the spec says is a `TypeError`. Inventing a NewTarget marker is a change
to the calling convention, not to these files. **`new String(...)` and
`new Boolean(...)` are therefore refused by name; decision 6 is why that is a
refusal rather than a recorded divergence.** `String(x)` and `Boolean(x)` —
which is what programs write, and the only form three.js writes — are exact.

`Boolean` was nearly free once the mechanism was in place, so it is here.
**`Function` is not, and deliberately.** `new Function(src)` compiles source at
run time, which an AOT compiler cannot do; a `Function` object that resolved and
then failed at the call would be a value lying about being callable. The name
stays an unresolved one, so it says so where it is used (docs/0027 decision 1).

## Decision 5 — `x instanceof Array` is answered as IsArray, exactly

`bronze_instanceof` walks the receiver's prototype chain. An array carries no
shape and therefore has no chain, so the walk returns false for every array —
which was invisible while `Array` was an unresolvable name and would have become
a silent wrong answer the moment it stopped being one, on one of the most
commonly written guards in JS.

So `instanceof` recognises the intrinsic `Array` constructor by code pointer and
answers IsArray. That is EXACT rather than approximate, and what makes it exact
is the next paragraph.

**`class X extends Array` is refused by name.** The derived constructor builds an
ordinary plain object and forwards to the base, but a native constructor ignores
the receiver and returns an object of its own (decision 2) — so `new X()` would
be a plain object that both `Array.isArray` and `instanceof Array` call false
while the program believes it made an array. Refusing is loud where allowing it
is silent, and it is what leaves no array in a bronze program whose prototype
chain could have made the two answers differ. The refusal covers the three
constructors this document adds; `Map`, `Set` and the typed arrays have the same
gap and it is older than this chunk.

`s instanceof String` and `b instanceof Boolean` need no such branch and are
already right: a primitive is never an instance of anything (13.10.2 step 5),
and bronze never builds a wrapper object for one to be an instance of.

## Decision 6 — a wrapper that does not exist is refused, in both directions

Decision 4 says `new String(x)` diverges. That was too soft: the divergence was
not a different answer, it was a **silent** one, and it fell out of the shape of
`bronze_construct` rather than out of any choice.

`bronze_construct` builds a plain instance, calls the constructor, and — per
13.3.5.1 — keeps the instance unless the constructor returned an OBJECT. A
native `String` returns a primitive, so the primitive was discarded and the
program received the empty plain instance:

```js
new String("a")            // {}
new String("ab").length    // undefined
new Boolean(false)         // {}, typeof "object", and truthy
```

That is a value lying about what it is, which is the exact failure decision 4
refused for `Function` — so it gets the same answer. **`new String(...)` and
`new Boolean(...)` are named hard errors** telling the caller to use the
conversion form. `Array` is deliberately not among them and must not be: 23.1.1.1
is ONE operation that reads NewTarget only to choose a prototype, so `Array(x)`
and `new Array(x)` build the same array and both are exact.

The other half of the same hole was on the read side. A property read on a
primitive boolean had **no branch at all** in the property path: it fell past
the string branch and the number branch into "not an object", and answered
`undefined` for everything. So `true.constructor` was `undefined` while
`(5).constructor` was a named error — two answers to one question, and the
silent one was the boolean's. The boolean now has its own branch, shaped like
the number's: `constructor` is answered with the same interned object the bare
name reads, and the two remaining members of `Boolean.prototype` (20.3.3 defines
exactly three) are diagnosed by name. A name the prototype does not define is
still `undefined`, because that is the language's own answer for a property that
is not there.

The wrapper objects themselves are pinned for when they land, in
`cases/blocked/primitive_wrapper_objects` — including the two facts that make a
silent `{}` so damaging: a String object indexes like its characters
(10.4.3.4), and `new Boolean(false)` is TRUTHY, because ToBoolean of an object
never looks inside it.

## Where the constructor objects live

Their own translation unit, `src/runtime/builtin_constructors.cpp`, named for
what it holds: the objects a bare NAME denotes. `builtin_array.cpp` and
`builtin_string.cpp` are headed `Array.prototype` and `String.prototype` and
answer a different question — "what can I call on a value?" — and a file that
had to explain both would name neither. The typed-array and `Map` constructors
stay where they are, beside header types that exist for nothing else.

## Named diagnostics

- `unsupported: Array.prototype is not implemented`, and the same for `String`
  and `Boolean` — decision 3. It covered only the three constructors in
  `kCtors`; `Map`, `Set`, `ArrayBuffer` and the nine views are interned
  singletons of their own and still reached the on-demand slot, so docs/0031
  decision 5 extended the refusal to them. The reason is decision 3's, unchanged:
  an empty object a program can install a method on is worse than an error.
- `unsupported: Array.fromAsync is not implemented`, `String.fromCodePoint`,
  `String.raw` — real statics ECMA-262 defines and bronze has not built, on the
  same rule as every other table: membership is "does this exist?", never "have
  we got round to it?".
- `RangeError: Invalid array length` — 23.1.1.1 step 3.b.
- `RangeError: Array allocation failed: N elements does not fit in the heap` —
  a length the specification allows and a semispace cannot hold.
- `TypeError: Array.from requires an array-like or iterable object, not <kind>`
  — 23.1.2.1 over null or undefined.
- `TypeError: Array.from: the second argument is not a function` — step 2.a,
  checked before anything is iterated so a bad mapper cannot half-consume the
  source.
- `extending the native constructor `X` is unsupported (its instances are built
  by the runtime, so a subclass would not be one)` — decision 5.
- ``new String(...)` is unsupported: bronze has no primitive wrapper objects.
  Call String(x) for the conversion, which is exact.`, and the same for
  `Boolean` — decision 6.
- `unsupported: Boolean.prototype.toString is not implemented`, and `valueOf` —
  the rest of 20.3.3, on the same rule as every other prototype table.

## What this unblocked

The three.js application runs to completion and prints its matrix. It is the
first time bronze has executed a real library end to end, and the numbers are
right: the 4x4 world matrix of a mesh after 60 frames of Euler rotation, built
through `Euler` → `Quaternion` → `Matrix4.compose`, matches the closed form of
the XYZ rotation to the last digit the quaternion round trip allows.
