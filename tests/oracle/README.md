# The oracle suite

Every behaviour question about JavaScript semantics is settled here, by
comparing a compiled program's stdout against pinned bytes. `oracle_test.cpp`
is the harness; it is a doctest suite like any other module's.

```
cmake --build --preset dev --target bronze_oracle_tests
ctest --preset dev -L oracle
ctest --preset dev -LE threejs      # skip the ~145s milestone case
```

## Adding a case

A case is a plain JS file that prints to stdout, plus a committed file holding
the exact bytes a correct run produces.

```
cases/<name>.js         cases/<name>.expected
cases/<name>/main.js    cases/<name>/main.expected     # a case that imports
```

For a directory case the directory is the case, `main.js` is what bronze is
pointed at, and its neighbours are what it imports. The pairing rule is the
same either way: the entry's path with the extension replaced.

Requirements the harness enforces:

- **Deterministic.** No clock read and no `Math.random` — checked by the
  harness, not by convention. A clock read is `Date.now()`, `new Date()` or
  `Date()`: the no-argument forms. The rest of `Date` is a pure function of its
  arguments and IS pinned here — `Date.UTC`, `Date.parse`, the field
  constructor, and every member of ECMA-262 21.4.4.
- **Timezone-independent.** A pinned expectation must hold on a machine in any
  zone, and no grep can check that. So a case pins UTC getters, ISO strings,
  epoch arithmetic and parses of strings carrying an explicit offset; it may
  exercise `getHours` and friends only inside a RELATION that holds in every
  zone (`getTimezoneOffset()` is an integer in [-1440, 1440]; a local field and
  its UTC twin differ by exactly that offset). Never pin an absolute local time.
- **An `.expected` file must exist.** A case without one is a failure, never
  a skip.
- **LF endings.** `.gitattributes` marks `*.expected -text` so CRLF
  conversion cannot corrupt a comparison.

Cases run under a 15s timeout and are killed on expiry, so a miscompiled loop
fails its case instead of hanging the suite.

Every case is compiled and run **twice — with inference and with
`--no-infer`** — and both must produce the same bytes. A case only inference
gets right means the dynamic path is unsound; a case only `--no-infer` gets
right means inference is. It doubles the suite's build cost and is what makes
the switch a bisection seam worth trusting.

## Deriving an expectation

By hand from ECMA-262, or from a one-off node run while authoring the case.
node is an authoring tool only — nothing in the build, the tests or CI invokes
it, and it is not a dependency.

## The ratchet

- A case is never removed or weakened once added.
- **An `.expected` file is never edited to make bronze pass.** A mismatch is a
  bronze bug, or occasionally a wrong hand derivation — and correcting a
  derivation means re-deriving from ECMA-262, never copying bronze's output.

## `cases/blocked/`

A case that cannot pass yet lives here with a header comment naming the
missing feature. **The suite asserts blocked cases FAIL**, so the day one
starts passing, promoting it is forced rather than optional.

Two conventions:

- **Promotion is not a file move.** Rewrite the header comment to describe
  what the case now pins, so a promoted case never reads as documentation of
  a bug that is gone.
- **Edge cases get their own file.** `array_methods_edges`,
  `string_methods_edges` and friends sit beside the cases they extend; a case
  that grows to cover everything stops naming what it is for.

Keep this directory non-empty. It is the list of what comes next, and an empty
one leaves the promote-on-pass ratchet with nothing to watch.
