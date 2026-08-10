# 0003 — Differential testing: node is the oracle

Status: starts alongside 0002 step 6; grows forever after.

bronze's correctness spine, carried from broc where it caught dozens of
real bugs: every behavior question is settled by running the same source
under node and under bronze and comparing stdout **byte-for-byte**. No
hand-written expected values for semantics; node IS the spec (V8's JS is
the ecosystem's de-facto semantics, and our product promise is "your JS,
native").

## Mechanics

- `tests/oracle/cases/*.js` — plain JS files that print to stdout. Each
  runs twice: `node case.js` and `bronze build && case.exe`. The test
  asserts byte equality. Cases must be deterministic (no Date/random —
  enforced by grep in the harness, same lesson as broc's determinism bugs).
- The harness is a doctest suite (`tests/oracle/oracle_test.cpp`) that
  shells out; node path resolved once, missing node is a hard failure not
  a skip.
- **Ratchet rule**: a case added is never removed or weakened. A case that
  cannot pass yet lives in `cases/blocked/` with a comment naming the
  missing feature — the suite asserts blocked cases FAIL (when one starts
  passing, promotion to `cases/` is forced, broc's promote-on-pass rule).
- Number formatting is the first battleground (JS's shortest-round-trip
  printing, negative zero, NaN/Infinity text). Get it exact in the phase-2
  runtime stub; every future type (strings, objects, arrays) adds its
  printing cases here before its implementation lands.

## Why stdout bytes and not exit codes / structured dumps

Bytes are the only interface node and bronze genuinely share, they compose
with every language feature ever added, and byte-compare failures bisect
fast (first differing byte → construct → module). Structured comparisons
(AST/IL dumps) stay in per-module tests; the oracle suite only ever speaks
stdout.

## Scale plan

Phase 2: ~10 numeric cases. Each language-growth phase (0001 phase 4) adds
its cases FIRST — the case list is the feature's spec. The end state is the
broc gate reborn with a better oracle: real libraries (three.js modules)
running under both and agreeing.
