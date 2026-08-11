# 0006 — Rooting generated code

Status: implemented. Completes decision 3 of
docs/0004, which accepted a shadow stack but only ever built the C++ half
of it. This doc settles the generated-code half: the frame ABI, the slot
discipline, and what is deliberately left slow.

## The hole this closes

0004 decision 3 says rooting discipline "lands with the FIRST allocation,
even while collection is a no-op bump allocator", and warns that
retrofitting roots onto a leak-and-forget runtime is the modern equivalent
of broc's refcount saga. The runtime half shipped — `ShadowStackFrame`,
`Rooted<T>`, a semispace collector that walks the frame chain — and the
runtime's own tests run under `--gc-stress`.

Generated code got nothing. Its `Dynamic` values live in LLVM SSA
registers that no root walk can see, so a collection that fires while any
of them is live moves the objects out from under them and leaves the
registers pointing into free from-space. The runtime said so in a comment and
reserved 512MB of heap to postpone the day:

> Reservation is virtual (commit is on demand), so reserve generously:
> generated code cannot yet survive a moving collection (its SSA values
> are unrooted — see docs/0004), so headroom delays that day of reckoning.

That is a silent fallback with a timer on it — precisely what the house
rules forbid. Nothing diagnoses the condition; a program simply allocates
past 512MB and corrupts. The 6.2x property-key interning win moved the
timer, not the bug.

## Decision 1 — frames are caller-allocated and linked inline

Generated code that holds any `Dynamic` value allocates, in its entry
block, one `bronze_gc_frame` (an LLVM `alloca`, so it costs a stack
adjustment and nothing else):

```c
typedef struct bronze_gc_frame {
    struct bronze_gc_frame* prev;
    uint64_t count;
    uint64_t slots[1];   /* count entries, inline */
} bronze_gc_frame;
extern bronze_gc_frame* bronze_gc_frame_top;
```

It initializes every slot to the `undefined` bit pattern, writes `count`,
and links itself onto `bronze_gc_frame_top` — three stores and a load, all
emitted inline. Before every `ret` it stores `prev` back to the head.
`Heap::collect` walks the list exactly as it walks the `ShadowStackFrame`
chain, forwarding each slot in place.

Both `bronze_gc_frame_top` and the struct live in the ABI registry
(`src/abi/bronze_abi.h`), which grows a `BRONZE_ABI_GLOBALS` list for data
symbols alongside the function list. Same rule as always: the backend
never hand-declares a runtime symbol.

- **Rejected: `frame_enter`/`frame_leave` helper calls.** This was the
  first implementation, on the theory that two calls per *rooted* function
  is cheap and inlining the list walk could wait. Measured: `fib` (a tiny,
  extremely hot, all-`dynamic` function — 2.7M calls) went from 118ms to
  248ms, **2.1x**, against 0004's promise of "a few percent". Inlining the
  link brought it back to 118ms exactly. The helper form is recorded here
  as the thing that does not work, not as a fallback.
- **Rejected: LLVM statepoints.** Already rejected in 0004 (invasive,
  thinly maintained, weakest on COFF/Windows). Nothing has changed.
- Frames are caller-allocated so entering costs no allocation — a heap or
  arena allocation per call would be absurd for a mechanism whose whole
  job is making allocation safe.
- `bronze_gc_frame_top` is deliberately **not** thread-local: TLS access
  from generated code is a real complication (model selection, COFF
  specifics) bought for a threading story bronze does not have. See "not
  in scope".

**Functions with no `Dynamic` values get no frame at all.** This is the
part that makes the cost acceptable and it falls straight out of 0004
("only `Dynamic` and reference-typed values get shadow-stack slots —
proven-f64 code carries zero GC obligation, so inference reduces root
pressure too"). `fib` and `numeric_loop` are pure f64 and must remain
byte-identical after this lands; that is a test, not a hope.

## Decision 2 — slot discipline: store at every def, load at every use

A root slot is only a root if it holds the *current* value. So for every
`Dynamic`-typed IL value:

- **at its definition**, store the freshly computed bits into its slot;
- **at every use**, load from the slot rather than reusing the register.

The load is what makes this correct: if a collection happened between the
def and the use, the slot was forwarded and the register was not. Block
parameters are defs (store at the top of the block); branch arguments are
uses (load at the terminator, after any call in that block).

This is deliberately conservative — a value that is never live across an
allocating call does not actually need a slot, and one that is never live
across *any* call does not need reloading. Computing that needs liveness
over the CFG. Not now: the memory form is correct by construction and
`-O2`'s store-to-load forwarding already collapses the redundant pairs
within call-free stretches. Decision 5 covers when to sharpen it.

## Decision 3 — call arguments live in the frame

`bronze_dynamic_call(callee, this, argc, argv)` takes a pointer to an
argument buffer that generated code builds. Those values are live across
the callee, so the buffer must be rooted, and the cheapest rooted memory
in the function is the frame itself. The frame is therefore

```
[ N value slots ][ maxArgc argv slots ]     count = N + maxArgc
```

where `maxArgc` is the widest call site in the function, and `argv` is a
pointer to slot `N`. One region serves every call site: IL is flat SSA, so
a call's arguments are written immediately before it and dead immediately
after — `f(g(x))` lowers to `g`'s call completing before `f`'s argument
list is built, never an overlap.

The callee's own `this` and callee values are ordinary `Dynamic` values
with ordinary slots.

## Decision 4 — collection points are exactly the ABI helpers

Generated code never allocates directly; every allocation is inside a
runtime helper it calls. So the safepoints are the helper calls, which is
what makes decision 2's "reload after any call" rule sufficient. Two
obligations follow, both on the runtime side:

- A helper that holds a `Value` across its own allocation must use
  `Rooted<T>`. Two places did not, and both are fixed with this work:
  `bronze_dynamic_call` copied its arguments into a `std::vector` — an
  *unrooted duplicate* of a buffer that is already rooted in the caller's
  frame, plus a malloc per dynamic call — and now passes the caller's
  buffer straight through. The lazily created `charCodeAt` function object
  was cached in a bare pointer that went stale on the first collection
  after any string touched it; runtime-owned caches like it now register
  with `Heap::add_permanent_root`, a root that outlives every frame.
- **Argument buffers must be consumed before the callee's first
  allocation.** This holds by construction — a callee's prologue stores
  its parameters into its own frame before it can call anything — and it
  is what lets the uniform calling convention pass raw `Value`s at all.
  Written down because nothing enforces it.
- `Shape::property_name` must point into the non-moving arena, never the
  moving heap: shapes are arena-allocated and are not scanned, so a shape
  holding a heap string would hold a stale pointer after the first
  collection.

## Decision 5 — the proof is a gc-stress run of the whole oracle suite

`--gc-stress` (collect on *every* allocation) already exists and the
runtime unit tests run under it. Compiled programs had no way to turn it
on, so the generated-code path — the one that was broken — was never
stressed.

- `Heap`'s constructor already reads `BRONZE_GC_STRESS` from the
  environment, so every compiled program has honoured it all along —
  nothing ever set it. Environment, not a compiled-in flag, so the *same*
  executable proves both paths and nothing about the generated code
  differs between the stressed and unstressed run.
- `ctest` gains an `oracle-gc-stress` run of the existing oracle suite
  with that variable set. Every `.expected` file must match byte-for-byte
  under stress. Under stress every single allocation moves every live
  object, so a missing root is not a rare race — it is a deterministic
  failure on the first case that allocates.
- This is a ratchet in the 0003 sense: it only ever grows, and a case that
  passes plain but fails stressed is a bug in this mechanism.

Only once that run is green does a perf number mean anything. Measured
after the run went green (logged in 0002): `numeric_loop` unchanged —
pure f64, no frame, exactly as designed; `fib` unchanged; `property_access`
~3% slower. That is the "few percent" 0004 promised.

The remaining sharpening, if a profile ever asks for it, is
**liveness-based slot allocation**: a value only needs a slot if it is
live across a call, and only needs reloading if something between its def
and its use could allocate. `fib` allocates 6 slots where 2 would do. It
is not worth the CFG liveness pass today. The real answer is inference
deleting the `Dynamic` values outright, which is the reason 0004 built the
shape model the way it did.

## Not in scope

- Multi-threading. The frame stack is thread-local, as `ShadowStackFrame`
  already is; bronze has no threads and no design for them yet.
- Interior pointers. Every rooted value is a tagged `Value`; generated
  code never holds a raw pointer into an object's interior.
- Unwinding. There are no exceptions (0005 defers `throw`/`try`), so a
  frame is popped by exactly one `ret` path. When exceptions land they
  must pop frames on the throw path, and that is that design's problem —
  named here so it cannot be forgotten.
