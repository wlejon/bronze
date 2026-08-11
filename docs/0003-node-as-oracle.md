# 0003 — Differential testing: pinned JS-semantics expectations

Status: live since 0002 step 6; grows forever after. Amended 2026-08-10:
the harness no longer invokes node at test time.

bronze's correctness spine, carried from broc where it caught dozens of
real bugs: every behavior question is settled by comparing bronze's
compiled output against ECMA-262 semantics **byte-for-byte** on stdout.
The spec bytes live in the repo as pinned `.expected` files.

## Why node is no longer in the test loop

The original design shelled out to `node case.js` on every test run.
That made the suite depend on a node install, made runs slower and
non-hermetic, and — decisively — when a miscompiled loop hung, the
`_popen` harness hung the whole suite with it (2026-08-10). Expectations
are now derived once (by hand from ECMA-262, or from a one-off node run
during case authoring), reviewed, and committed. node remains a useful
*authoring* tool for producing an expectation candidate; it is not a
test-time dependency and nothing in the build or CI invokes it.

## Mechanics

- `tests/oracle/cases/<name>.js` — plain JS files that print to stdout.
  Each has a committed `cases/<name>.expected` holding the exact stdout
  bytes of a semantically correct run. The harness compiles the case
  with bronze, runs the executable, and asserts byte equality with the
  expectation. Cases must be deterministic (no Date/random — enforced by
  grep in the harness, same lesson as broc's determinism bugs).
- `.expected` files are byte-exact: LF line endings, protected from CRLF
  conversion by `.gitattributes` (`*.expected -text`).
- Compiled cases run under a **hard timeout** (15s) and are killed on
  expiry — a miscompiled loop fails the case, never hangs the suite.
- Every case is compiled and run **twice: with inference and with
  `--no-infer`**, and both must produce the same pinned bytes (docs/0010
  decision 8). This is what makes the switch a ratchet rather than a
  comfort blanket — a case only inference gets right means the dynamic
  path is unsound, and a case only `--no-infer` gets right means inference
  is. It doubles the suite's build cost, which is the price of never
  having to guess which half a failure came from.
- The harness is a doctest suite (`tests/oracle/oracle_test.cpp`); a
  case without an `.expected` file is a hard failure, not a skip.
- **Ratchet rules**: a case added is never removed or weakened, and an
  `.expected` file is never edited to make bronze pass — a mismatch is a
  bronze bug or (rarely) a wrong hand derivation, and correcting a
  derivation requires re-deriving from ECMA-262, not from bronze's
  output. A case that cannot pass yet lives in `cases/blocked/` with a
  comment naming the missing feature — the suite asserts blocked cases
  FAIL (when one starts passing, promotion to `cases/` is forced,
  broc's promote-on-pass rule).
- Number formatting was the first battleground (JS's ToString(Number)
  rules, negative zero under console.log, NaN/Infinity text). Every
  future type (strings, objects, arrays) adds its printing cases here
  before its implementation lands.

## Why stdout bytes and not exit codes / structured dumps

Bytes compose with every language feature ever added, and byte-compare
failures bisect fast (first differing byte → construct → module).
Structured comparisons (AST/IL dumps) stay in per-module tests; the
oracle suite only ever speaks stdout.

## Scale plan

Each language-growth phase (0001 phase 4) adds its cases FIRST — the
case list is the feature's spec. The end state is the broc gate reborn:
real libraries (three.js modules) compiled by bronze producing pinned,
reviewed output.

**2026-08-11**: `cases/blocked/` had been emptied by the promotions of
phases 2 and 3, which left the promote-on-pass ratchet with nothing to
watch and — worse, given the rule above — left no case list describing what
comes next. It is seeded again with the first three of phase 4:
`math_builtin` (bronze has no `Math` at all, so three.js cannot begin),
`array_methods` and `string_methods` (every real member of those prototypes
is a named hard error today). All three deliberately route their results
through `join` or print scalars, because `console.log` of a container has no
pinned format yet (docs/0009) and a blocked case must not smuggle that
decision in through an `.expected` file.

**2026-08-11 (later)**: all three of those promoted, along with
`template_literals`, `arrow_functions` and `for_of` — the syntax cases
seeded next to them (docs/0012). Promotion is not a file move: each case's
header comment described the error it produced while blocked, and that
sentence is rewritten to describe what the case now pins, so a promoted case
never reads as documentation of a bug that is gone. Three edge cases were
added alongside them rather than folded in (`array_methods_edges`,
`string_methods_edges`, `string_escapes`), because a case that grows to
cover everything stops naming what it is for.

`cases/blocked/` keeps `class_basics`, which is the next thing three.js
needs and which docs/0012 decision 5 plans out.
