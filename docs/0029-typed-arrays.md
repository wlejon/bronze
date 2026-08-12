# 0029 — Typed arrays: a byte store in a moving heap

Status: implemented, except decisions 4 and 6.

docs/0004 promised this: *"Typed arrays are first-class, early — `Float32Array`
is the beating heart of three.js. `ArrayBuffer` = GC object owning a byte
buffer; views = `{buffer ref, byteOffset, length, element type}`."* What
existed before this doc was one element type, reachable only through a `new`
that lowering recognised by name. Across three.js there are ~109 uses spread
over all nine view types, `ArrayBuffer` and `DataView`, and the single line
standing between bronze and a running three.js application was

```
Uncaught ReferenceError: Uint32Array is not defined
```

## Decision 1 — the buffer is an ordinary MOVING heap object, and a view holds an offset

This is the decision everything else inherits, so it comes first.

A byte store has two properties nothing else in the heap has: its payload
must never be scanned as `Value`s, and something else (the view) has to be
able to address a position inside it. A copying collector (docs/0004 decision
3) is hostile to both.

**Accepted: the buffer is an ordinary heap object with `Tag::RawBytes`, and a
view holds `{Value buffer, uint32 byteOffset, uint32 length, uint32 kind}`.**
The element address is recomputed as `buffer.data() + byteOffset + i * width`
at every single access. No data pointer is ever cached across anything that
can allocate.

- The collector already understands `RawBytes`: `Heap::collect`'s scan loop
  skips the payload of a `String` or a `RawBytes` object, so the bytes are
  copied verbatim and never mistaken for tagged values. A `float` whose bit
  pattern happens to read as `0xFFF1…` — a plausible object pointer — is
  therefore not "forwarded" into garbage. That is pinned by a runtime test
  that plants exactly those bit patterns and collects.
- The view holds the buffer as a `Value`, so the ordinary payload scan keeps
  it alive and rewrites the reference when it moves. Nothing about a view
  needs a root source, a finalizer, or a write barrier.
- Two views over one buffer therefore keep agreeing across a collection, for
  free: they both name the same object, and the object's identity is what the
  collector maintains.

**Rejected: a non-moving allocation the views point into directly.** A raw
`malloc`'d block with the views holding real pointers is faster to address
and is what an engine with a mark-sweep old generation would do. It is wrong
here for reasons that are structural and not stylistic:

- A semispace copying collector has **no sweep phase**, so there is nowhere
  to run a finalizer. The buffer would either leak or need a side table of
  live buffers reconciled after every collection — a second ownership model
  next to the one the whole runtime is built on, which is what docs/0000
  calls the refcount saga.
- three.js allocates a buffer per `BufferAttribute` per geometry. Leaking
  them is not a corner case, it is the steady state.
- 48-bit `Value` payloads work because *the whole heap is ours and reserved
  low* (docs/0004 decision 1). A second allocator would put buffer addresses
  outside that reservation, so a buffer could not be a `Value` at all, so it
  could not be an ordinary JS object — and `v.buffer` is observable.

**The cost, stated plainly:** a live 30 MB buffer is memcpy'd on every
collection. That is what a semispace collector does to all live data, and the
fix when a benchmark asks for one is a large-object space or a generational
arrangement — not a second ownership model bolted on now.

**The obligation this puts on every line of code that touches a view**: a
`TypedArrayHeader*` or a `uint8_t*` is valid only until the next allocation.
Every loop in `builtin_typed_array_methods.cpp` that can allocate — the two
that call back into user code, and the one that can throw — re-derives both
pointers from a root on each step. The ones that cannot are marked as such,
because the safe and unsafe versions look identical.

### `byteLength` 0, and `byteLength` huge

- **Zero** is legal and is a real object: `new ArrayBuffer(0)` has
  `byteLength` 0, views over it have `length` 0 and print `Float64Array(0)
  []`. It is also the smallest `Object`-tagged allocation in the runtime,
  which is why `BRONZE_ABI_OBJ_MIN_PAYLOAD` is pinned against it — generated
  code's inline property fast path loads the header and the shape word from
  any object-tagged pointer *before* it knows what kind of object it has.
- **Huge** is two separate errors, deliberately kept apart. A length that is
  not a non-negative integer below 2^53-1 is 7.1.22 ToIndex's `RangeError`
  ("Invalid typed array length"). A length that is fine by the specification
  but cannot be allocated is a different `RangeError` naming the heap
  ("Array buffer allocation failed: N bytes does not fit in the heap"),
  raised *before* the allocation, so `std::bad_alloc` never unwinds out of a
  helper that generated code called. A buffer must fit in a semispace,
  because decision 1 says a collection copies it.
- `kMaxByteLength` is 2^28. It is far below the heap-fit bound, and it is not
  arbitrary: the view header's `{byteOffset, length}` pair shares one 8-byte
  word that the collector's payload scan reads as a `Value`, so a `length` at
  or above `0xFFF1_0000` would present a valid pointer TAG and be
  "relocated" — the length would be overwritten with an address. The cap is
  three orders of magnitude below that, which is what makes the shared word
  safe rather than lucky. `reserved` exists to keep the word above `kind`
  a non-pointer for the same reason.

## Decision 2 — a constructor is a real global object, interned by code pointer

Measured, in `three/src/math/MathUtils.js`:

```js
switch ( array.constructor ) {
    case Float32Array:  return value;
    case Uint32Array:   return value / 4294967295.0;
    ...
}
```

So the bare name `Float32Array` and `someView.constructor` must be **the same
object**, and `===` must hold between them. A `new` form recognised by name —
which is what bronze had — cannot express any of that, and `new
source.array.constructor( source.array )` in `BufferAttribute.copy` cannot be
expressed by name recognition at all, because the callee is a member
expression.

The mechanism, and chunk 18 inherits it:

1. **Lowering's closed provided-globals list** (`Lowerer::isProvidedGlobal`,
   docs/0011 decision 1) gains the eleven names. A free identifier on that
   list lowers to `global.get "<name>"`; everything else keeps the
   unresolved-name treatment of docs/0027. Shadowing falls out of where the
   check sits — it is the last resort in identifier lowering — so `const
   Float32Array = ...` still wins with no special case.
2. **`bronze_global_get` resolves the name** through
   `rtTypedArrayConstructor`, which returns
   `bronze_function_singleton(code, 0)`. That helper interns on the CODE
   POINTER, so the same name always yields the same object, and the global
   cache's entries are already a GC root source.
3. **Nine distinct code pointers** come from nine instantiations of one
   function template over `ElementKind`. That is the whole trick: one body,
   nine identities. A single function taking a kind parameter would have
   given all nine constructors the same code pointer and therefore the same
   interned object, and `Int8Array === Uint8Array` would have been true.
4. **`v.constructor` asks the same function** for the constructor of the
   view's own element kind, so it cannot answer with a different object. This
   is the 10.2.5 back-pointer chunk 11 installed for ordinary functions and
   chunk 14 extended to the `Error` family, reached the only way a header
   type without a shape can reach it: as a branch in the property path.
5. **Arity 0**, for the reason docs/0011 decision 2 gives: `FunctionHeader::
   arity` is the count a short call is PADDED to, and a variadic native must
   see the real `argc` — with arity 3, `new Float32Array(buf)` would arrive
   as `(buf, undefined, undefined)` and take the wrong branch.

`new Float32Array(...)` now goes through the ordinary `Op::Construct` helper
like every other constructor. Two IL opcodes (`create.f32array`,
`create.arraybuffer`), two ABI entries and the name-recognition branch in
`lowerNewExpr` were **deleted**, not added to. That is the shape of the
answer chunk 18 should copy for `Array`, `String`, `Boolean` and the rest:
put the name on the list, hand back an interned function object, delete the
special case.

Three divergences, all shared with `Map` and `Set`, all recorded rather than
papered over:

- Calling one **without `new`** is a `TypeError` in 23.2.5.1 step 1, and
  bronze builds the array instead. A native constructor cannot see NewTarget
  through the uniform calling convention (docs/0004), and inventing a marker
  for it is a change to the convention, not to this file.
- **`v instanceof Float32Array` is false.** `instanceof` walks the receiver's
  prototype chain, and a view carries no shape, so it has no chain to walk —
  the same reason `m instanceof Map` is false (docs/0021). What a program
  actually uses to ask this question is `v.constructor`, and that is exact.
- **`Float32Array.prototype` reads as an empty object** rather than the named
  error it should be, because the function branch of the property path
  answers `prototype` from the `FunctionHeader` before it consults any
  unimplemented-member table. That is pre-existing and not specific to typed
  arrays — `Map.prototype` has behaved the same way since docs/0021, whose
  `kMapUnimplemented` entry for `prototype` is unreachable for exactly this
  reason. Installing a method on it would be found by nothing.

## Decision 3 — one implementation, parameterised by element kind

`ElementKind` is a stored `uint32` field on the view, not nine header types
and not nine header flags. Everything else reads a table:

| what | where |
| --- | --- |
| name (`"Uint8ClampedArray"`), bytes per element | `kElementKinds`, in the enum's order |
| load | `TypedArrayHeader::get` — one `switch`, `memcpy` per width |
| store, with the conversion | `TypedArrayHeader::set` → `convertForStore` |
| constructor object | `kCtors`, one entry per kind |
| printed name | the same `name` field (docs/0013) |

`HeapObjectHeader::flags` stays `3` for **every** view, which is what let the
nine land without touching a single one of the existing receiver-kind
dispatches: `bronze_prop_get`, `bronze_elem_get`, `in`, `for-in`, the
iterator's fast kind, `console.log` and the inline-cache guard in generated
code all still ask one question and get one answer. Encoding the kind in
`flags` instead would have turned every `flags == 3` into a range test, and
`flags` already means five other things.

The conversions are ECMA-262 7.1.6..7.1.11 and are written **once**:
`toIntegerModulo(value, bits, isSigned)` covers all six integer kinds, so
they cannot disagree about what `1e40` narrows to (the answer is 0 for all
six — the double is an exact integer divisible by 2^80, which no modulus here
survives; a C cast would have said something else). `ToUint8Clamp` is its own
function because it saturates and rounds half to **even**, the one place JS
does not round half away from zero. Three subtleties are pinned by tests
because each is a plausible wrong answer:

- `-0.5` into an integer kind is `+0`, not `-0`. 7.1.6 step 3 truncates the
  *mathematical* value. A `-0` stored here would read back as `-0` and print
  as `-0` (docs/0013 decision 1), announcing a sign the conversion does not
  produce.
- `2.5` clamped is `2` and `1.5` is `2`.
- `Float32` narrows by storing: `(float)` is IEEE round-to-nearest-even,
  which is what 6.1.6.1 specifies, and re-widening is exact — so `0.1` reads
  back as `0.10000000149011612` in a `Float32Array` and as `0.1` in a
  `Float64Array`.

Byte ORDER is the platform's. 10.4.5.5 stores through `SetValueInBuffer` with
`isLittleEndian` left to the implementation, and only `DataView` lets a
program name it. Every target bronze builds for is little-endian; that is
what `typed_array_buffer_views` pins, and it is recorded here rather than
discovered in an `.expected`.

## Decision 4 — the indexed fast path is NOT here, and this is what it needs

`a[i]` on a `Float32Array` is what three.js does in its inner loops, and
docs/0004 calls lowering it to a raw indexed load "a headline perf target,
not a compatibility checkbox". It is **not built**, and pretending otherwise
would be worse than saying so. Today `v[i]` is `bronze_elem_get` — a call, a
`valueToElementIndex`, a flags dispatch, a `switch` on the kind, a `memcpy`
and a box. The box is free (docs/0004 decision 1) and everything before it is
not.

**The baseline, measured** (`bench/typed_array_loop.js`, best of three,
2000 × 1024 iterations of `sum += v[i]; v[i] = v[i] * k`, so 6.1M element
operations; process startup is the ~33 ms the pure-f64 control costs):

| loop | best | per element op |
| --- | --- | --- |
| `Float32Array` elements | 469 ms | ~71 ns |
| plain JS array elements | 637 ms | ~98 ns |
| the same arithmetic, no container at all | 33 ms | — |

So the container costs about **14x** the arithmetic it carries, and a typed
array is already 1.35x a plain array because its element path has no `Value`
to unbox and no hole to check. Those are the numbers the fast path has to
beat, recorded here so the next chunk starts from a measurement rather than
from this paragraph.

What was examined, and what it would actually take:

- **The lattice has no element for it.** `types::TypeKind` has `Object` with
  an optional shape class and nothing that can carry "a view of `Float32`
  elements". docs/0010's own "not here" list says exactly this: *"Typed array
  element access lowering to raw loads … needs the view's element type
  proven, which this doc's lattice can carry, but the lowering is its own
  work."* The lattice can carry it; it does not yet.
- **The producer is easy and the propagation is the work.**
  `FlowAnalyzer::newExpr` already special-cases a bare-name callee to intern
  a shape class, and the nine names are now exactly the bare names that
  matter, so `new Float32Array(n)` would type as `TypedArray(Float32)` in a
  few lines. What is not a few lines is everything downstream: `join` rules,
  the loop fixpoint, `subarray`/`slice`/`map` returning the same kind, and a
  `BufferAttribute`'s `this.array` field — which is a *property*, and
  docs/0010 decision 4 proves receiver shape classes and deliberately never
  proves a property's type. Without that last piece the analysis proves
  nothing about `attribute.array[i]`, which is the loop that matters.
- **The consumer is a guard, not a proof.** Even with the type proven, the
  guard stays, exactly as it does for property reads (docs/0010 decision 7):
  the proof is over this compilation's source and the header is the runtime's
  authority. The emitted form is the same shape as `emitPropGet`'s inline
  arm — `tag == Object`, `flags == 3`, `kind == K`, `idx < length` — and then
  a load of `buffer`, an add of `byteOffset`, and one `fpext`/`sitofp`. Four
  compares and two loads inline, against a call today.
- **A kind-blind inline path was considered and rejected for now.** Guarding
  only `flags == 3` and then switching nine ways on `kind` inside generated
  code is nine basic blocks per site, which is a lot of IR to spend on a site
  that inference could have narrowed to one block.

So: the honest answer is that this chunk made the fast path *possible* — one
header, one flag value, a kind field at a fixed offset, and constructors that
inference can now recognise by name — and did not build it. It is the first
thing to do next, and it is a chunk with a benchmark, not a paragraph in this
one.

## Decision 5 — the methods are the ones three.js calls, and no more

Built: `set`, `subarray`, `slice`, `fill`, `copyWithin`, `forEach`, `map`,
`indexOf`, `includes`, `join`, `[Symbol.iterator]`, and the properties
`length`, `byteLength`, `byteOffset`, `buffer`, `BYTES_PER_ELEMENT`,
`constructor` (plus `BYTES_PER_ELEMENT` on the constructor, 23.2.6.2).
`ArrayBuffer` carries `byteLength` and `constructor`.

Everything else on `%TypedArray%.prototype` stays a **named hard error**
naming the receiver's own constructor (`Uint8Array.prototype.sort is not
implemented`), never `undefined` — docs/0011 decision 3, with the table moved
next to the members that answer so a name leaving the list and a name
arriving are one edit.

Two shapes are worth calling out because they are one character apart and
mean opposite things:

- `subarray` returns a view over the **same** buffer at an offset; a write
  through either is visible in the other.
- `slice` returns a view over a **new** buffer; it is a copy.

`set` clones its source unconditionally when the source is another typed
array. 23.2.3.26.1 step 15 only requires it when the two share a buffer, but
the comparison that would avoid the copy ("do these overlap?") is one that is
easy to get subtly wrong for a partial overlap between views of different
widths, and getting it wrong corrupts data silently.

`for-of` and spread do **not** reach `[Symbol.iterator]`: `rtOpenIterator`
recognises a typed array and steps a cursor (docs/0021 decision 2). The
iterator object exists for a program that pulls it out by hand, and
`typed_array_iteration` pins that both routes produce the same values.

`Symbol.species` is not modelled, so `slice` and `map` produce a view of the
receiver's own element kind. Nothing in three.js subclasses a typed array.

## Decision 6 — `DataView` is not built, and is diagnosed by name

One use in three.js, and it is the one view that cannot share any of the
above: `getFloat32(1, true)` has no alignment requirement and an explicitly
named byte order, so it cannot use the offset-times-width addressing every
`%TypedArray%` element access is. `DataView` is not on lowering's globals
list, so the name is an unresolved one — a compile-time warning and a
`ReferenceError` where it is evaluated (docs/0027 decision 1), never a silent
`undefined`. `cases/blocked/typed_array_dataview` holds the pinned behaviour
for when it lands.

## Printing

docs/0013 owns the format and is extended there rather than here: the
constructor's name, the length in parentheses, then the elements —
`Uint8ClampedArray(3) [ 0, 255, 7 ]`. `ArrayBuffer` remains the named error
docs/0013 decision 3.4 made it.

## Named diagnostics

- `RangeError: Invalid typed array length` / `Invalid array buffer length` —
  7.1.22 ToIndex step 2.c.
- `RangeError: typed array allocation failed: length is too large` — past
  `kMaxByteLength`.
- `RangeError: Array buffer allocation failed: N bytes does not fit in the
  heap` — decision 1's semispace bound.
- `RangeError: start offset of Float32Array should be a multiple of 4`,
  `byte length of Float32Array should be a multiple of 4`,
  `Start offset N is outside the bounds of the buffer` — 23.2.5.1 step 6.
- `RangeError: offset is out of bounds` — 23.2.3.26 `set`.
- `TypeError: %TypedArray%.prototype.<m> called on a value that is not a
  typed array` — the ValidateTypedArray step.
- `TypeError: %TypedArray%.prototype.set takes a typed array or an array` —
  bronze reads a source's elements directly and has no general array-like
  read; a silent no-op over a `length` it cannot see would be worse.
- `unsupported: <Kind>Array.prototype.<name> is not implemented` — decision 5.
- `named property writes on a typed array (<Kind>Array) are unsupported` — a view has no
  shape, so there is nowhere to put one; discarding it would leave the
  program believing it stored something.
- `printing an ArrayBuffer is not implemented` — docs/0013 decision 3.4.
