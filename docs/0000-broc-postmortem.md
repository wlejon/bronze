# 0000 — Predecessor post-mortem: broc

Every "broc" reference in these docs resolves here. This is self-contained;
no access to the broc repo is needed (it exists at `D:\projects\broc`,
read-only, if you must dig).

## What broc was

A JS-to-native AOT compiler implemented in TypeScript (2026-08, retired
2026-08-10). Pipeline: acorn parse → SSA IR (".form", canonical text form)
→ verifier → passes → x64 MASM → ml64+link against a C runtime backed by
QuickJS. It reached real milestones: the compiler compiled all 93 of its
own modules; the native-compiled compiler (broc.exe, 13.6MB) ran real
programs and produced output byte-identical to the node-run compiler; a
subset self-compilation fixpoint held (broc.exe compiled its own text
package byte-identically and the result linked and ran).

## Why it was retired

1. **Wrong value representation (the fatal one).** QuickJS `JSValue` was
   the universal substrate: every object/string/property op in compiled
   output was a C-API call into QuickJS at dynamic-JS prices. Measured:
   ~50x slower than node on compiler workloads — losing to V8 *and*
   plausibly to interpreted QuickJS. Static layouts existed but were the
   exception, with no incremental bridge between "proven layout" and
   "generic object". bronze inverts this (docs/0001, 0004).
2. **Refcounting in generated code.** Generated code initially never freed
   QuickJS values → an unbounded leak (a full-tree self-compile hit 94GB
   RSS and climbing). The fix (per-function value-pool frames) bounded it
   but pinned ~5x node's live set, leaving the full-tree fixpoint
   memory-infeasible (12GB+ where node used 1GB). Lesson: RC correctness
   as a per-callsite obligation of a code GENERATOR is a losing game →
   bronze chooses a precise tracing GC (0004 decision 3).
3. **TS as implementation language** brought node heap ceilings, an
   eval-face/native-face split, and a slow, runaway-prone iteration loop.

## Bugs that shaped bronze's standing rules

- **Silent module drop**: broc's IR text parser consumed one module and
  returned; nothing checked EOF; multi-module files lost modules 2..N
  silently for days. → Rule: every parser consumes ALL input or
  hard-errors (bronze enforces this in `Parser::parseModule`).
- **`localeCompare` in a canonical printer** → output depended on the
  machine's locale. → Rule: deterministic output only — no locale, no
  hash-map iteration order, floats via `to_chars`.
- **`0 ?? x` returned `x`**: an evaluator treated 0 as nullish. Caught
  only by differential testing against node. → Rule: node is the oracle
  (0003); hand-written expectations don't catch semantics you
  misremember.
- **NUL-in-string-literal truncated**: the emitted string table stored
  bare NUL-terminated char* with no length, so "\0" == "". Caught by a
  byte-identity ratchet at the fixpoint. → Rule: byte-for-byte
  differential ratchets at every stage; a one-byte diff is a real bug.
- **Silent fallbacks generally** (an optimizer pass wrapped in
  try/catch-ignore; a backend emitting "; fallback op:" comments for
  unknown IR). Every one eventually lied to us. → Rule: hard errors with
  named constructs; unimplemented ≠ silent no-op.

## What carries forward (methodology, not code)

- Canonical text form per stage + byte-compare tests ("ratchets": pinned
  lists only grow; a blocked case that starts passing MUST be promoted).
- Scoped per-module tests during iteration; one full proof before commit.
- Heavy dependencies isolated so daily iteration never rebuilds them
  (broc-era pain: everything rebuilt everything).
- Differential testing against a trusted oracle beats hand-written
  expectations, always.
