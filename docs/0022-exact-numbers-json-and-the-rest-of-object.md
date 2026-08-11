# 0022 — Exact numbers, JSON, and the rest of `Object`

Status: implemented. The second half of phase 4's builtins (docs/0001), and
the syntax hole that was blocking them from being written readably.

Four things that look like four chunks and are one, because the thing they
have in common is the house rule they are each a test of: **a plausible answer
that is not the right one is worse than a loud error.** `toFixed` through
`printf`, `JSON.stringify` through `console.log`'s formatter, `JSON.parse`
through `src/parse`, and `setPrototypeOf` through an inline cache that never
notices are four different ways to ship the same bug, and each of them looks
finished from the outside.

docs/0021 named three of them in "what this chunk stopped short of" and
`cases/blocked/json.js` and `cases/blocked/number_methods.js` held the pinned
bytes. Both are promoted.

## Decision 1 — object-literal method shorthand, and why its IL symbol has dots in it

`{ next() { ... } }` was `unsupported construct: object literal method
shorthand`. It is how every iterator, every options object and most of
three.js is written, and docs/0021 had to spell `next: function ()` in its own
test cases to work around it.

The property a method defines is indistinguishable from the one
`m: function () {}` defines — own, enumerable, writable, holding an ordinary
function object (13.2.5.5 → 15.4.5 with `enumerable: true`) — so lowering
needed no change at all. Two things in the parser did.

**`super` belongs to the home object.** A method's `super` resolves against the
object the method was defined in, and bronze resolves `super` by the class name
the parser is currently inside (docs/0012 decision 5). An object literal inside
a class method is a different home object, so inheriting that binding would
make `super.m()` in `{ go() { return super.m(); } }` silently call the
enclosing class's parent. `parseMethodTail` therefore saves and CLEARS
`inClassMethod_` / `currentClassSuper_` around the body, which turns the case
into the existing "super outside a class method" error — the honest answer
until bronze has a home-object model.

**The IL symbol is `obj.<n>.<name>`, and the dots are load-bearing.** Lowering
registers every function it creates in `functionIndices_`, keyed on its name,
and a free identifier resolves through that map. A method named `next` named
`next` there would answer a free `next(...)` elsewhere in the module — a method
name is a property key and never a binding. The ordinal is because two literals
in one module both writing `m()` would otherwise agree on a symbol, and the
second definition would silently replace the first. `cases/object_method_
shorthand.js` pins both halves with a module function and a method that share
a name.

Method shorthand is accepted for every PropertyName form — an identifier, a
reserved word, a string literal and a computed key — because they are one
production (15.4) and building three of the four would leave a hole shaped like
nothing.

## Decision 2 — every digit of `toFixed` comes from integer arithmetic

21.1.3.3 asks for "the integer n for which n / 10^f − x is as close to zero as
possible", where x is the **real number the double denotes**. The double
nearest 1.005 is 1.00499999999999989341858963598497211933… exactly, so that n
is 100 and `(1.005).toFixed(2)` is `"1.00"`. Every implementation that reaches
for `printf("%.*f")`, `std::format` or a `std::to_chars` round-trip answers
`"1.01"` for at least some inputs, because those round the SHORTEST DECIMAL
that identifies the double rather than the double itself. That is a silent
wrong answer in precisely the code — money, report columns — that calls
`toFixed` at all.

So `src/runtime/exact_decimal.{h,cpp}` computes `round(|x| × 10^k)` with no
floating-point step in the middle: the double is split into `m × 2^e` with no
rounding, both powers are moved into a numerator and a denominator so that
everything stays an integer, and the division is binary long division over a
little-endian 32-bit bignum with the tie resolved away from zero. The widest
value any caller builds is about 1400 bits and this runs once per formatted
number, so schoolbook is the right algorithm and Knuth's algorithm D — whose
normalization step is the part of a bignum divide that is easiest to get subtly
wrong — is not.

One helper answers all three of the decimal methods, because 21.1.3.2 step
10.a and 21.1.3.5 step 10 are the same search with a different width: find the
`e` for which `round(x × 10^(digits−1−e))` has exactly `digits` digits.
`std::log10` seeds it and the DIGIT COUNT of an exact result corrects it, so a
floating-point estimate that is off by one costs an iteration and never an
answer.

`toString(radix)` is the one that does not fit. 21.1.3.6 leaves a
non-terminating fraction's digit count implementation-defined, and getting it
right means a shortest-round-trip algorithm in that radix. bronze answers only
where the answer is exact: an integer in any radix, and a fraction in a
power-of-two radix, where a double's dyadic fraction terminates. `(0.1)
.toString(3)` is a named error rather than a plausible-looking approximation.

`tests/runtime/exact_decimal_test.cpp` proves the arithmetic directly rather
than only through the oracle case's console output, including 2^−1074 scaled
back to a 757-digit integer — a value no double multiply survives.

The wrapper methods are reached the way `String.prototype`'s already were: the
property path hands them out for a primitive receiver (docs/0011 decision 2).
That also closed a silent fallback nobody had noticed — a property read on a
number answered `undefined` for every name, so `(1.5).toFixed(2)` died as
"undefined is not a function" instead of naming the member. It now has a
`Number.prototype` table in the docs/0011 decision 3 sense.

## Decision 3 — JSON's grammar is not JavaScript's, so it does not get JavaScript's parser

`src/json` is a module: code units in, a tree out, and no idea bronze's value
model exists. It links against nothing, `tests/json` drives it with no heap at
all, and its whole reason to be separate is what it **rejects**. A trailing
comma is legal in a JS object literal and a syntax error here; so are `'x'`,
`{a:1}`, `0x10`, `+1`, `.5`, `1.`, `01`, `1e`, `// comment`, `\x41`,
`undefined`, `NaN`, `Infinity` and a raw newline inside a string. Sharing a
parser between two grammars that differ in what they refuse is how the
refusals get lost.

Two details of the grammar are written out rather than folded:

- The member and element loops demand a KEY or a VALUE after a comma rather
  than looping until the closing bracket. The fold is the idiomatic way to
  write it and it is exactly how a trailing comma becomes accepted.
- A number's parts are checked before `strtod` sees them, because `strtod`
  accepts `1.`, `.5` and hex.

The input is UTF-16 code units, not UTF-8, because a JSON string element is a
code unit and `"\ud800"` is a legal JSON text. Re-encoding on the way in would
lose the lone surrogate or replace it with U+FFFD.

The tree is ordinary C++ memory holding no heap pointers, which is what lets
the conversion to values allocate freely — only the values already built need
rooting, and each level of the walk roots exactly the one it is filling.

## Decision 4 — `JSON.stringify` shares no code with `console.log`, on purpose

docs/0013 chose an inspect format with deliberate divergences from node,
because its audience is a human reading a terminal. This output is data another
program parses, so it has no divergences available to it, and the two therefore
share nothing: quoted keys, `"a"` where inspect writes `'a'`, no space unless
`space` asked for one, `null` for NaN and both infinities, and `0` for `-0`
where inspect writes `-0`.

The splits worth naming, all of them 25.5.2:

- `undefined`, a function and a symbol are **omitted** from an object and
  written as `null` in an array. That is the one place the two containers
  differ, and it is why `SerializeJSONProperty` returns "no value" rather than
  a string, leaving the caller to decide which it means.
- `toJSON` is consulted on any object, through the ordinary property machinery
  so an inherited one is found.
- The replacer function sees the ROOT too, under the empty key, with a real
  wrapper object as its receiver — so a replacer that reads `this[""]` works.
- QuoteJSONString is defined per code unit: a well-formed surrogate pair passes
  through whole and an unpaired surrogate is escaped, because the result must
  be well-formed text that re-parses to the same string.
- Key order is own-enumerable order, which docs/0009 already fixes and
  `bronze_object_keys` already answers. This file never sorts.

Circular-structure detection holds POINTERS into the rooted slots the recursion
already owns, rather than copies of the values. A copy would name where an
object used to be after the first collection inside a `toJSON` call, and the
symptom would be a missed cycle rather than a crash.

## Decision 5 — `setPrototypeOf` is made safe by dictionary mode, not by a wider cache

An inline cache entry is `(shape, slot, depth)` and a hit is taken when the
RECEIVER's shape matches. Nothing in it notices a change to an object BETWEEN
the receiver and the holder — which is the bug `cases/blocked/proto_chain_
invalidation.js` pins for an add to an intermediate prototype, and which a
prototype swap is a second, worse instance of: after
`Object.setPrototypeOf(mid, other)` a cached depth-2 entry walks two links,
lands on a different object entirely, and reads an unrelated slot.

The real fix widens `InlineCache`, which widens the stride
`src/codegen-llvm/llvm_prop.cpp` inlines, which is a generated-code contract
change and its own chunk. This one is not that, and shipping a stale read was
not an option. So:

- **`Object.setPrototypeOf` moves its target to dictionary mode** and repoints
  that private shape's root at a root shape carrying the new prototype. Only
  the root of a transition tree stores a prototype and a dictionary shape
  belongs to exactly one object, so the swap moves that object's prototype and
  nobody else's.
- **`ObjectHeader::cachedProtoHolder` refuses a walk that crosses a dictionary
  at ANY link**, not only at the holder. docs/0019 decision 5 checked the
  holder, which was enough while `delete` was the only thing that made a
  dictionary; a swap makes one in the middle.

The second half is what makes the first sound, and it costs one pointer load
per link on a path that was already walking those links. It also closed one
line of the blocked case as a side effect — after a delete leaves a dictionary
on the walk, a later add at the nearest prototype is now seen — which is noted
in that file as an accident of ordering rather than a fix, because it is one.

The fill site asks the same walk rather than restating the condition, so the
hit path and the fill path cannot drift into disagreeing about one entry.

`Object.create` needs a root shape per prototype, and it is **memoized on the
prototype's identity** — otherwise every object created in a loop would carry a
hidden class no inline cache had ever seen, and each call would leak an
immortal arena shape. The memo needs no roots of its own: it compares against
the shape's own prototype slot, which the collector already forwards. A table
keyed on a private copy of the prototype would be decision 6's problem all over
again.

## Decision 6 — `Object.getPrototypeOf({})` is a named error, because bronze has no `Object.prototype`

A plain `{}` in bronze has no prototype at all. The language says its prototype
is `Object.prototype`, and answering `null` would be a wrong answer that reads
exactly like the RIGHT answer for `Object.create(null)` — two different facts
collapsed into one byte, which is the shape of bug this project's rules exist
to prevent. So the two are kept apart: an explicit null prototype answers
`null`, and the absence of an intrinsic is a named error. The same holds for an
array and a function, whose prototypes are the two other intrinsics bronze does
not have.

`cases/blocked/object_intrinsic_prototypes.js` holds the expectations for the
real thing and names the work: real prototype objects, root shapes pointing at
them, the property path finding methods THROUGH them rather than beside them,
and enumeration unaffected because everything on them is non-enumerable. That
is the chunk that makes monkey-patching work, and `hasOwn`, `is` and
`getOwnPropertyDescriptors` wait for it rather than landing unpinned beside it.

## Decision 7 — the Map index's epoch counts RELOCATIONS, and has a witness

docs/0021 decision 4 made a Map's bucket index record `Heap::collection_count()`
and rebuild when the number moved on, because an object key hashes by its
address and a semispace collector changes addresses. docs/0021's own report
flagged the hole, and it is real: the counter was incremented at the END of
`collect()`, so any future second entry point — a nursery sweep, a compaction,
anything that moves objects without finishing a full cycle — would move objects
while leaving the count alone. Every Map index in the program would then answer
"not found" for a live key, and no existing test would catch it, because the
linear-scan oracle drives collections through `collect()` too.

Two changes, and the point of having two is that neither depends on anyone
remembering the rule:

- The counter is now `Heap::relocation_epoch()`, and it is incremented **by the
  copy that relocates an object**, three lines into `forward_value`. Moving an
  object past it now requires writing a second object-copy routine, not merely
  a second collection driver. `collection_count()` remains, documented as
  statistics that nothing about correctness may hang off.
- The index also records the map's **own address**. That is the independent
  witness: to relocate anything a collector must trace from the roots, a live
  map is on that trace, and moving the map changes this number whatever the
  collector does about counters. Neither check subsumes the other — an epoch
  catches a map that happened to land back on its old address, and the anchor
  catches a collector that bypassed the epoch — so the index is valid only when
  both agree.

`tests/runtime/map_test.cpp` pins the property directly rather than through the
linear-scan comparison, precisely because that comparison cannot see it: the
epoch moves per object moved, allocation alone does not move it, and a
collection with nothing live to move does not move it either.

## What this chunk stopped short of

- **`o.k++` and `a[i]--`.** `cases/blocked/property_update_operators.js`. Found
  while writing an iterator — `this.i++` is how every one of them is spelled —
  and left named rather than built inside a chunk about builtins: it is a
  lowering change with an evaluation-order contract of its own.
- **The intrinsic prototype objects**, per decision 6.
- **`"abc"[0]`.** A computed index on a string is still `computed index access
  on a non-object value is unsupported`. It is a hard error and not a wrong
  answer, so it waits for the chunk that gives strings a prototype.
- **`Number.prototype.toString` of a fraction in a radix that is not a power of
  two**, per decision 2.
- **`JSON.stringify` of a Map or a Set** answers `{}`, which is what ECMA-262
  says (they have no own enumerable string properties) and is the classic
  surprise in every engine. It is pinned as correct, not as a limitation.

## Errors this chunk added

- `unsupported: Number.prototype.<name> is not implemented` — decision 2's
  table, currently `constructor` and `toLocaleString`.
- `toFixed() digits argument must be between 0 and 100`, and the two
  equivalents for `toExponential` and `toPrecision`; `toString() radix must be
  between 2 and 36`. All RangeErrors, all catchable.
- `Number.prototype.<name> called on a value that is not a number` — TypeError.
- `unsupported: Number.prototype.toString(<r>) on a value with a fraction`.
- `SyntaxError: <what was expected> at position <n>` from `JSON.parse`.
- `Converting circular structure to JSON` — TypeError.
- `Object prototype may only be an Object or null`, `Property descriptors must
  be an object`, `Object.<m> called on a value that is not an object` —
  TypeErrors.
- `unsupported: Object.getPrototypeOf of a plain object needs Object.prototype,
  which bronze does not provide`, and the array/function form — decision 6.
- `unsupported construct: ++/-- on a property (write `o.k += 1`)`, which
  replaced `invalid update operand` — the construct is ordinary JavaScript
  bronze has not built, and the old message read as a syntax complaint.

## Files this chunk added, and the seams it cut

- `src/json/{json.h,json.cpp}` and `tests/json` — the grammar, and only the
  grammar. It is a module rather than a file because the isolation rule is what
  guarantees it cannot quietly start sharing `src/parse`'s permissiveness.
- `src/runtime/exact_decimal.{h,cpp}` — the bignum and the exact scaled
  rounding. The seam is "what is the exact decimal expansion of this double"
  against "what does 21.1.3.3 do with it", and the first half is provable
  without a heap.
- `src/runtime/builtin_number_proto.cpp` — the wrapper methods, split from
  `builtin_number.cpp` along the line ECMA-262 itself draws between the
  `Number` namespace's statics and `Number.prototype`'s methods.
- `src/runtime/json_stringify.cpp` and `src/runtime/builtin_json.cpp` — the
  pinned byte format, and the namespace plus the parse surface. Two names, so
  two files.

`src/cli/driver.cpp` gained one thing worth flagging: the list of static
libraries compiled output links. CMake does not merge archives, so a module the
runtime depends on has to be named there too, and that list is now the one
place that coupling lives.
