# 0004 — The `dynamic` value model

Status: amended and accepted 2026-08-10 (supersedes the same-day draft;
review added encoding invariants, reversed the string decision, and added
the arrays / prototypes / functions / enumeration sections).

Wild JS must compile (0001 decision 4), so a dynamic representation is
unavoidable — the design goal is that it is (a) fast enough that
inference misses degrade gracefully, and (b) fully ours, so specialization
can bypass it field-by-field rather than all-or-nothing. This is THE
foundation choice broc got wrong (QuickJS JSValue as substrate → 50x).

## Decision 1 — value representation: NaN-boxing (accepted)

64-bit `Value`; **doubles are stored as themselves** (SpiderMonkey
orientation, not JSC's re-biased doubles — bronze is numbers-first, so the
numeric path pays nothing and pointers pay the mask). Pinned invariants:

- **Canonical NaN**: `0x7FF8_0000_0000_0000`. Every site that boxes a
  computed double canonicalizes (`if (v != v) v = canonical`) so arbitrary
  arithmetic NaN payloads can never alias a tag. Raw f64 arithmetic in
  typed code is untouched — canonicalization happens only at box sites.
- **`isNumber(v)` is one unsigned compare**: `bits(v) <= 0xFFF0_0000_0000_0000`
  (the bit pattern of -Infinity; all non-NaN doubles and the canonical NaN
  fall at or below it). Unbox-to-f64 is a no-op bit reinterpret.
- **Tags** live in the upper 16 bits, `0xFFF1..0xFFFF`; payload is the low
  48 bits. Pinned tag table (tests pin these numbers; they never reorder):
  `0xFFF1` Object, `0xFFF2` String, `0xFFF3` Int32 (reserved, see below),
  `0xFFF4` Bool, `0xFFF5` Null, `0xFFF6` Undefined, `0xFFF7` Hole
  (internal: array holes / TDZ; never user-visible), `0xFFF8` Symbol
  (reserved). Singletons: `null`/`undefined`/`hole` have payload 0;
  `true`/`false` payload 1/0.
- **Pointer payloads fit 48 bits by construction**: the GC heap (decision
  3) is ours, so the runtime reserves it via `VirtualAlloc` inside a fixed
  low range (< 2^47). Unbox-pointer is a single AND of the low 48 bits.
  This coupling is a stated reason the GC must be ours — no masking
  gymnastics, immune to 5-level-paging address growth.
- **Int32 tag is reserved but unimplemented.** Doubles represent every
  int32 exactly and typed paths carry the numeric load; the fast path
  lands only when a benchmark demands it.

`il::Type::Dynamic`'s ABI is this u64. It is fixed before the first
`dynamic` op is lowered.

## Decision 2 — object model: shapes (hidden classes), ours (accepted)

Objects = shape pointer + slot storage (fixed inline slots + out-of-line
overflow array). Shapes form a transition tree on property insertion
order. The load-bearing idea: **an object PROVEN by inference to have a
fixed layout uses the same slot storage** — a specialized access is just
"skip the shape check", so proven and unproven code share one heap model
and interop is free. This is the bridge broc lacked; it makes inference
pay incrementally instead of bimodally.

Sharpenings (part of the accepted design, not optional):

- **AOT inline caches are data, not patched code**: a per-site
  `{shape*, slot}` word pair checked before the load. Monomorphic only
  until measured. ICs are the *midpoint* — the endpoint is inference
  deleting the check entirely; don't gold-plate them.
- **Shapes live in a non-moving arena** (decision 3's collector moves
  objects; shapes never move). IC words need no GC fixup and shape
  identity stays a raw pointer compare, forever.
- **Dictionary mode is designed in from the start**: `delete` and
  pathological property churn transition an object to a dictionary-shaped
  form (hash lookup, no transition tree). Wild JS uses `delete`; without
  this escape hatch it becomes a silent-fallback temptation later, which
  the house rules forbid. May be implemented late, but the transition
  point is part of the shape design, and hitting an unimplemented
  dictionary transition is a named hard error until it lands.
- **Prototypes**: the shape records the prototype pointer; changing
  `__proto__` is a shape transition (or dictionary demotion). Own-miss
  walks the proto chain; proto-hit ICs cache `{receiver shape, holder,
  slot}`. Method calls are property lookups — no separate mechanism.
  **Built — see docs/0008**, which refines the IC to cache a *depth*
  rather than a holder (a holder is a movable pointer, which would have
  contradicted the "no GC fixup" line above) and adds the `this` / `new`
  surface without which none of this is reachable from source.
- **Enumeration order is spec'd JS order and deterministic**: integer
  keys ascending, then string keys in insertion order (recoverable from
  the shape's transition chain). `Object.keys` / `for-in` print through
  the oracle, so this is a correctness surface, not a nicety.
  **Built — see docs/0009**, with `Object.keys` as the surface; `for-in`
  remains a named hard error, and the dictionary boundary is marked and
  diagnosed rather than silently paid for.

## Decision 3 — memory management: tracing GC, precise, ours (accepted)

The broc leak/pin saga is the cautionary tale for refcounting in generated
code: RC emission correctness is a per-callsite obligation forever. A
precise tracing GC moves the obligation into one module.

- **Shadow stack for roots, explicitly not LLVM statepoints.** Statepoints
  are invasive, thinly maintained, and weakest on COFF/Windows. A shadow
  stack costs a few percent and is entirely ours; revisit only if profiles
  demand it. Only `Dynamic` and reference-typed values get shadow-stack
  slots — proven-f64 code carries zero GC obligation, so inference reduces
  root pressure too.
- **Semispace copying to start**, shapes give exact slot maps. Measure,
  evolve.
- **Rooting discipline lands with the FIRST allocation**, even while
  "collection" is a no-op bump allocator. A `--gc-stress` mode (collect on
  every allocation) exists from the moment collection does, and the
  runtime test suite runs under it. Retrofitting roots onto a
  leak-and-forget runtime is the modern equivalent of broc's RC saga.
- **Non-moving arena** for shapes and other runtime metadata (decision 2).
- C++ runtime code roots through a scoped handle API (`Rooted<T>`), never
  raw pointers across allocation points.

Alternatives on record: RC+cycles (rejected: broc), conservative Boehm
(rejected: unpredictable pauses, no compaction, hostile to shapes).

## Strings — dual representation, exact JS semantics (reverses the draft)

The draft's UTF-8 proposal is rejected: its "deviate only where the
oracle can't tell" clause doesn't hold — the oracle CAN tell, trivially,
for any non-ASCII string (`.length`, `s[i]`, `charCodeAt`, `slice` are
UTF-16-code-unit semantics), and JS strings can hold lone surrogates,
which valid UTF-8 cannot encode. "Node is the spec" (0003) does not get
its first exception in the string type.

Accepted design, the same one production engines converged on:

- **Immutable, length-prefixed, dual representation**: Latin-1 (one byte
  per code unit) when every code unit ≤ 0xFF, UTF-16 otherwise. Most
  real-world strings are ASCII, so this keeps UTF-8's memory win with
  exact node-observable semantics.
- Header: length in code units (u32), flags (rep, hash-computed), hash
  cache; data inline after the header. `.length` is the stored length.
- Rope/slice representations only when a benchmark demands them.
- Oracle cases for non-ASCII length/indexing and surrogate splitting land
  in `cases/blocked/` BEFORE string implementation starts (0003: the case
  list is the spec).

## Arrays and typed arrays (added; three.js is the bar)

- **JS arrays**: an object (shape for named props) + a separate dense
  elements store (`length`, capacity, `Value[]`). Holes are the internal
  Hole singleton, never user-visible. Because of NaN-boxing, a packed
  `Value[]` of numbers IS a flat f64 array already — element-kind
  specialization (packed-double etc.) is deferred until a benchmark asks.
  Out-of-range / sparse writes beyond a threshold: named hard error until
  dictionary elements land (no silent slow-mode).
  **Amended 2026-08-11**: "an object (shape for named props)" describes the
  design, not what shipped — an array header carries no shape, so a *named*
  write (`a.foo = 1`) fell off the end of the element path and was silently
  discarded, and the subsequent read of `a.foo` answered `undefined`. That
  is the silent slow-mode this bullet forbids one line later, so it is now
  `named property writes on an array are unsupported (arrays carry no shape
  for named properties yet)`, matching what the Float32Array branch beside
  it already did. Giving arrays a shape is the fix; diagnosing is the
  boundary marker until then.
- **Typed arrays are first-class, early** — `Float32Array` is the beating
  heart of three.js. `ArrayBuffer` = GC object owning a byte buffer;
  views = `{buffer ref, byteOffset, length, element type}`. Element
  access on a proven-typed view must lower to a raw indexed load/store —
  this is a headline perf target, not a compatibility checkbox.

## Functions and closures (added)

- A JS function is an object (shape — three.js attaches properties to
  functions) holding a code pointer + environment pointer.
- **Uniform dynamic calling convention**:
  `Value fn(Value env, Value thisArg, uint32_t argc, Value* argv)`; callee
  handles arity adaptation (missing args read as undefined). Proven call
  sites bypass this entirely with direct typed calls. (The `env` parameter
  was added by 0007; this section originally had no room for it.)
- Captured variables live in GC-allocated environment records; escape
  analysis promoting captures to registers/stack is inference's job,
  later. **Built — see docs/0007**, which resolves the questions this
  paragraph left open: one environment per *scope* rather than per
  function, and the environment reaching the callee through the calling
  convention.

## What phase 2 needed from this doc

Nothing — phase 2 was all f64 and is complete. Decision 1's encoding is
now pinned and is the first thing the runtime module implements; boxing
round-trip tests pin the tag table byte-for-byte.
