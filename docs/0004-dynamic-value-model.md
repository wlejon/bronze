# 0004 — The `dynamic` value model (decisions needed before phase 4)

Status: design draft — the three decisions below want sign-off before any
object/string code lands. This is THE foundation choice broc got wrong
(QuickJS JSValue as substrate → 50x); bronze must get it right in the open.

Wild JS must compile (0001 decision 4), so a dynamic representation is
unavoidable — the design goal is that it is (a) fast enough that
inference misses degrade gracefully, and (b) fully ours, so specialization
can bypass it field-by-field rather than all-or-nothing.

## Decision 1 — value representation: NaN-boxing (recommended)

64-bit values; doubles are themselves; pointers/ints/bools/null/undefined
live in the NaN space. Rationale: JS numbers are doubles and our f64 fast
path must be zero-cost when a `dynamic` turns out numeric — NaN-boxing
makes unbox-to-f64 a single compare. Alternative (tagged pointer + boxed
doubles) rejected: allocates on the hottest path in numeric-heavy wild JS.

## Decision 2 — object model: shapes (hidden classes), ours

Objects = shape pointer + inline slot array. Shapes form a transition tree
on property insertion order; inline caches at property sites (even in AOT
code: a per-site cache word checked before the slot load). Critically, an
object PROVEN by inference to have a fixed layout uses the same slot
storage — a specialized access is just "skip the shape check", so proven
and unproven code share one heap model and interop is free. This is the
bridge that makes inference pay incrementally instead of bimodally
(broc's layouts-vs-QuickJS split had no such bridge).

## Decision 3 — memory management: tracing GC, precise, ours (recommended)

The broc leak/pin saga is the cautionary tale for refcounting in generated
code: correctness of RC emission is a per-callsite obligation forever.
A precise tracing GC (semispace to start, shapes give exact slot maps,
LLVM statepoints or a shadow stack for roots) moves the obligation into
one module. Start simple: shadow stack + semispace copying, measure, evolve.
Alternatives to keep on record: RC+cycles (rejected: broc), conservative
Boehm (rejected: unpredictable pauses, no compaction, hostile to shapes).

## Strings

Immutable, length-prefixed UTF-8 (not UTF-16, deviating from JS observable
behavior ONLY where the oracle suite can't tell the difference; `.length`
and index semantics need a code-unit answer — a UTF-16-length cache field
covers the common cases; revisit with oracle cases before phase 4 strings).
Rope/slice representations only when a benchmark demands them.

## What phase 2 needs from this doc

Nothing — phase 2 is all f64. But `il::Type::Dynamic`'s ABI (the NaN-box
u64) should be fixed before the first `dynamic` op is lowered, so decision
1 is the first one to lock.
