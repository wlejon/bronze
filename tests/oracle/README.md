# The oracle suite

Every behaviour question about JavaScript semantics is settled here, by
comparing a compiled program's stdout against pinned bytes. `oracle_test.cpp`
is the harness; it is a doctest suite like any other module's.

```
cmake --build --preset dev --target bronze_oracle_tests
ctest --preset dev -L oracle                # differential cases, built and JIT (excluding milestones)
ctest --preset dev -L jit                   # the JIT half alone
ctest --preset dev -LE "threejs|pixi"       # fast loop (skip both heavy milestones)
ctest --preset dev -L threejs               # three.js r160 milestone, built and JIT
ctest --preset dev -L pixi                  # pixi.js v8 milestone, built and JIT
ctest --preset dev -L jit-milestone         # the two milestones through `bronze run` only
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
  harness, not by convention. A clock read is `performance.now()`, `Date.now()`,
  `new Date()` or `Date()`: the no-argument forms. The rest of `Date` is a pure
  function of its arguments and IS pinned here — `Date.UTC`, `Date.parse`, the
  field constructor, and every member of ECMA-262 21.4.4.
  `performance.now()` has no pure half at all, so it is banned outright and
  tested in `tests/cli/performance_now_test.cpp` instead, where the assertions
  are relations (monotonic, sub-millisecond, finite) rather than pinned bytes.
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

Every case is ALSO run through **`bronze run`** (the `oracle-jit` test): the
source compiled in-process by the JIT and run in that same process, which is
how the bro engine runs an app that has no `app.dll`. Its stdout must match
the same pinned bytes, and its exit code and stderr must match the built
program's — the built program and the JIT load the same brass object, so a
divergence lives in what surrounds it (entry and exception propagation, hook
and host-global installation, symbol resolution, module init order), which is
exactly what neither half can see alone. Each JIT run is a subprocess of the
harness, for the same three reasons the built program is one: the runtime is
process-global with no teardown, so a second program in one process would see
the first one's globals; a miscompiled loop is stopped by killing the process
that runs it; and a crash fails its case instead of the suite. The gc-stress
run is repeated for the JIT too, and there the compile happens inside the
stressed process.

One thing the JIT half cannot pin, and the built half cannot either: dynamic
code compilation (`Function("...")`, `eval`, and the generator/async
constructors' call forms). node compiles it, `bronze run` compiles it through
the runtime's dynamic hooks, and a built program — with no compiler beside
it — refuses with a TypeError. A case must not call them;
`tests/cli/cli_test.cpp` pins the refusal on the built program alone.

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
