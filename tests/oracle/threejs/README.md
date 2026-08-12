# The three.js milestone case

`three/` holds three.js **r160** source, byte-for-byte as published. It is the
transitive import closure of `main.js`'s five entry classes — `Scene`,
`PerspectiveCamera`, `Mesh`, `BoxGeometry`, `MeshBasicMaterial` — and nothing
else: 28 files, ~225 KB, laid out at the paths the library's own relative
specifiers name, so every `import './MathUtils.js'` inside it resolves without
a shim.

## Why the library is vendored rather than approximated

The bar in `CLAUDE.md` is "three.js must compile". A hand-written program that
exercised the same mechanisms — an ES module graph, classes, prototype chains,
typed arrays, generators, `Math.random`, the global constructors — would have
passed on the day before three.js first ran, because it would have been written
against what bronze already did. Only the library's own source can fail for the
reason that matters. It is the one test in the repo whose subject nobody on this
project wrote.

The closure is the smallest thing that keeps that true. Vendoring all of
three.js would multiply the compile cost without widening what is proven;
vendoring less would mean patching import statements, and a patched library
proves only that the patch works.

## What it costs, and where it runs

One compile of the graph is ~70 s, which is why this case is not in `cases/`:
the oracle suite compiles every case twice per ctest test and there are two
ctest tests, so a case dropped into `cases/` would be compiled four times. It
lives here instead, under the ctest test `oracle-threejs` (label `threejs`),
which compiles it once per inference mode and runs it three times — inference
on, `--no-infer`, and once more with `BRONZE_GC_STRESS=1` against the
already-built executable, which costs half a second and is where a missing GC
root in generated code shows up.

`.\dev.cmd ctest --preset dev -L threejs` runs only this;
`.\dev.cmd ctest --preset dev -LE threejs` is the fast pre-existing loop.

## Updating the vendored copy

Re-copy from a pristine release tree, do not hand-edit. If a newer revision
changes the library's arithmetic, re-derive `main.expected` from the new source
— never from what bronze prints.

## License

three.js is Copyright © 2010-2024 three.js authors, MIT licensed. The full text
is in `three/LICENSE`.
