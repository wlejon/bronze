# 0031 — The milestone as a pinned case, and the branches that fell off their ends

Status: accepted.

Unmodified three.js compiles and runs. Nothing in the suite protected that,
which meant every chunk after it could take the milestone away silently. This
records how it is now held, and — since the same pass read across the seams
sixteen feature chunks left — which quiet answers were made loud on the way.

## 1. The library is vendored, not approximated

A hand-written program exercising the same mechanisms (a module graph across
many files, classes, prototype chains, typed arrays, generators, `Math.random`,
the global constructors) was the cheaper option and was rejected. Such a program
would have been written against what bronze already does, so it would have
passed on the day *before* three.js first ran; it can only fail for reasons its
author anticipated. The library cannot. It is the one test in the repo whose
subject nobody here wrote, and that is the entire value of it.

What is vendored is the transitive import closure of the five entry classes and
nothing else — 28 files, ~225 KB, at `tests/oracle/threejs/three/`, byte for
byte as published (r160, MIT). The closure is the smallest thing that keeps
"unmodified" true: vendoring more would multiply the compile cost without
widening the proof, and vendoring less would mean rewriting import specifiers,
after which the case proves that the rewrite works.

The corollary is a standing rule. **The vendored tree is never edited.** If it
has to move to a newer revision, it is re-copied whole and the expectation is
re-derived from the new source — never from what bronze prints.

## 2. What an expectation may say when the subject is floating-point

The obvious pin — the world matrix after 60 frames — is the one thing that
must not be pinned. It is deterministic for a fixed compiler, so it *would*
hold; but the bytes could only come from observing bronze, and docs/0003's
ratchet exists precisely to keep an expectation from being a record of the
implementation's behaviour.

So the case prints only three kinds of thing:

- **Integers and identities** that follow from the library's own source.
  `BoxGeometry(1,1,1)` builds six planes at one segment each, so it has
  `6 * (1+1)^2 = 24` positions and `6 * 2 * 3 = 36` indices. `mesh.parent`
  is `scene`. `position.array.constructor` is `Float32Array`. None of these
  has a rounding question in it.
- **Arithmetic that IEEE-754 performs exactly.** `Matrix4.compose` with an
  identity quaternion reduces to `diag(sx,sy,sz)` plus a translation column;
  with integer positions and scales, every product and difference in it is
  exact, and so is the 4x4 multiply that composes a parent with a child. The
  case pins those element strings in full, because they are derivable from the
  source rather than observed.
- **Invariants, where the value really is an accumulation.** A rotation matrix
  has orthonormal columns, determinant 1, and an inverse that composes back to
  the identity; `updateMatrixWorld` is a pure recomputation, so running it twice
  cannot move anything. These are pinned as the booleans they are, inside
  `1e-12` — four orders of magnitude above double rounding over the tens of
  flops involved, and ten orders below the ~1e-2 that any wrong operand, wrong
  order or dropped store would produce.

`Math.random` is reached (three.js draws a uuid in `Object3D`'s constructor and
in `BufferGeometry`'s) and is never printed. What is pinned is that the uuid is
a 36-character string and that two independent draws differ, which is what
separates a working generator from a constant.

## 3. The cost is paid once per mode, not four times

Compiling the graph takes ~70 s. The oracle suite compiles every case twice —
inference and `--no-infer` — and two ctest tests share the binary, so a case
dropped into `cases/` would be compiled four times and the suite would go from
263 s to over 500 s.

It therefore lives beside `cases/` rather than in it, under its own ctest test
`oracle-threejs` with its own label `threejs`, driven by a doctest name filter
so that no second harness had to exist. It compiles once per mode (~145 s) and
runs three times: inference, `--no-infer`, and once more against the executable
the inference build already produced, with `BRONZE_GC_STRESS=1`. That third run
costs half a second, because the collector reads the variable in the *compiled
program*, not in the compiler — and it is the dimension that has caught a
shipped rooting bug three times.

It IS in the default run. A test excluded from `ctest --preset dev` is a test
nobody runs, and the milestone is worth more than the 55% the suite grows by.
`-LE threejs` is the fast loop for iterating; `-L threejs` is the milestone
alone.

The determinism grep the other two suites apply to a case's source cannot apply
here, because the library calls `Math.random`. What replaces it is stricter and
is enforced by the expectation itself (decision 2 above), not by a scan.

## 4. `o.k` and `o[k]` are one question, so they get one dispatch

`bronze_elem_get` held a second copy of `propGetByName`'s receiver-kind
dispatch, and every place the two copies had drifted was a silent wrong answer:

- `arr[k]` for `"push"` was `undefined` where `arr.push` was the method, and
  the same for `"length"` and `"constructor"`.
- `Math[k]` for an unbuilt member was `undefined` where `Math.cbrt` was a named
  error, because the computed path skipped the four namespace checks.

Both are gone, and not by patching the copies into agreement. `bronze_elem_get`
now keeps exactly the two things that justify its existence — the integer index
fast path for an array and for a typed array — and hands everything else to
`propGetByName` with the key converted to a string. A duplicated invariant is
not a style problem; it is a pair of answers waiting to disagree.

## 5. A branch that ends without a named error is the bug

Six read paths ended by returning `undefined` (or, worse, a default object)
where the honest answer was "bronze does not implement this". They are one
shape, and the rule that finds them is: *does this return mean "the property is
absent", or does it mean "the search never happened"?* ECMA-262 10.1.8 makes
the first `undefined`; the second is CLAUDE.md's silent fallback.

Made loud:

- **Indexing a string.** `"abc"[0]` was `undefined` while `"abc"[i]` was already
  a hard error — one operation, two answers, and the quiet one was wrong.
  Refused by name in both spellings now. `cases/blocked/string_index` pins the
  behaviour it must be replaced by, so the refusal cannot outlive its excuse.
- **`prototype` on an intrinsic constructor.** `Map.prototype`,
  `Set.prototype`, `ArrayBuffer.prototype` and the nine views answered a *fresh
  empty object*, created on demand by the `FunctionHeader` — worse than
  `undefined`, because a method installed on it succeeds and is then found by
  nothing. docs/0030 had already drawn this conclusion for `Array`, `String`
  and `Boolean` and guarded only those three.
- **A property read on a value with no branch.** Symbols and the array-hole
  sentinel reached a bare `return undefined`. That is the exact shape that let
  `true.foo` read as `undefined` until docs/0030 wrote the boolean branch.
- **A property WRITE to a primitive** was discarded, two lines below a comment
  saying that discarding a write is worse than reading `undefined`. A module is
  strict code (16.2.1.6), so 6.2.5.6 PutValue throws.
- **The plain-object tail** cast any unrecognised object kind through an
  `ObjectHeader`. `bronze_elem_get` used to hold that guarantee for the computed
  path and lost it when the two paths merged, so the guard moved with it.

## 6. A name bronze declared and failed to bind is bronze's error

docs/0027 decision 1 draws the line at what bronze can PROVE: an unknown global
is unprovable and gets the spec's runtime `ReferenceError` plus a warning, while
an unknown `console` member is provable and is refused now. One diagnostic was
on the wrong side of it.

`function g(){ if (true) { var j = 6; } return j; }` reported `warning:
unresolved name 'j'` and threw at run time. But `j` is declared — 8.6.2 hoists a
`var` to the enclosing FUNCTION at any block depth, and bronze creates the slot
only for the ones written at the top level. So a compiler gap was wearing a
language error's costume, which is the failure mode `namedFunctionExprs_` was
built to prevent for the other declared-but-unbound name. It now takes the same
route: a compile error that names the hoisting.

The hoisting itself is not fixed here — that is a feature, and docs/0028 notes
that `ast::functionBindsName` is deliberately coupled to it, so whoever widens
one must widen the other in the same change.

## 7. `argv` is not self-protecting, and the fourth rooting bug

Three chunks had shipped the same rooting bug, each caught only by a late
`oracle-gc-stress` run. This is the fourth, and it was found by looking for the
shape rather than by tripping over it.

`bronze_rest_args` allocated its output array **before** it read a single
element of `argv`, and `emitCallWrappers` deliberately read every named
parameter **after** building the `arguments` object — both on a written-down
premise: *"`argv` needs no protection: it points into the CALLER's frame, which
the collector updates in place."*

That premise is true exactly when the caller is generated code, whose argument
block lives in its GC root frame. It is false when the caller is a **builtin
calling back into JS**. `Array.prototype.map`, `forEach` and `reduce`,
`Array.from`'s mapper, the JSON replacer and reviver, the regexp replacer and
`FunctionHeader::call`'s arity-adaptation vector all build their block in plain
stack memory that `Heap::collect` never scans and never updates — and
`RootedArgs`' own comment already said so, three lines from the code that
assumed the opposite.

So `items.map(function (...a) { return a[0].n; })` collected inside `newArray`,
moved the objects the caller had written into that block, and read forwarded
headers for the rest of the call. It answered `1,1` instead of `1,2`. Silently:
`forward_value` overwrites only the tag word and the first payload word, so the
value still looks like an array or a string and reports a garbage length.

Both halves take the same fix — **read and root before you allocate, never
after**. `bronze_rest_args` now opens with the `RootedArgs` copy its own
contract demands, and the wrapper loads every named parameter first and carries
them through the root frames that the two array builds already establish.

The rule this leaves behind is worth more than the fix: **an argument block is
only as rooted as its CALLER, and half of bronze's callers are builtins.** Any
helper that reads `argv` after an allocation is wrong, whatever the comment
above it says.

`cases/callback_args_across_collection` pins it, and it is in the default suite
precisely so `oracle-gc-stress` runs it every time.

## 8. No file was split

The threshold this pass was briefed against was 800 lines. The largest files are
`src/ast/queries.cpp` (768), `src/parse/parser_expr.cpp` (717),
`src/regex/parser.cpp` (708), `src/runtime/rt_prop.cpp` (696 before this pass)
and `src/runtime/builtin_string_regexp.cpp` (680) — none over it, and none with
a seam that names something rather than a place to cut.

`rt_prop.cpp` was the one examined hardest, because it held three of the six
silent answers above. It has a clean read/write seam, and splitting on it was
rejected: the read and write sides share the key helpers, and the file's real
problem was never its length but the duplicated dispatch inside it, which
decision 4 deleted. A split would have moved that duplication into two files and
made it harder to see. An unnecessary split is a large diff that hides a dropped
assertion, and the cheapest way to lose a pinned test is to move it.
