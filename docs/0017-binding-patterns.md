# 0017 — Binding patterns, defaults, rest and spread

Status: implemented. Part of phase 4 of docs/0001.

Six constructs, one mechanism. A default value, a rest parameter, a spread,
a destructuring pattern, a derived class's implicit constructor and a
destructuring `for-of` head all answer the same question: **how does a value
get from where it is written to the names that bind it, when the count is
not fixed and the shape is not a single name?**

They landed together because they are not separable. `...rest` is what makes
`f(...args)` meaningful; `super(...args)` is what a derived class with no
constructor desugars to, so the class case cannot land without spread; and
the `undefined`-and-only-`undefined` rule is shared between a parameter
default and a default inside a pattern — split across two chunks it would
have been written twice and drifted once.

Before this chunk, `function f(a, b = 2)` was reported as
`expected ')' after parameters`, `...` was `unsupported construct: spread`
wherever it appeared, `const [a, b] = pair` was
`unsupported construct: destructuring declaration`, and `class D extends B {}`
was a named error because forwarding needs rest and spread. Three blocked
oracle cases (docs/0003) held the hand-derived expectations for all of it;
all three are promoted by this chunk.

## Decision 1 — a default is a branch on strict `undefined`, evaluated per call

The rule ECMA-262 10.2.11 states, and the one most implementations of this
feature get subtly wrong, is that a default fires when the argument is
`undefined` **and in no other case**. `f(1, null)` binds `null`. `f(1, 0)`
binds `0`. `const { z = 5 } = { z: null }` gives `null`, because the
property is present and holds `null`. A truthiness test here is wrong for
four of JavaScript's seven falsy values.

So the lowering is a `strict.eq` against `const.undefined`, and never a
truthiness check.

It is also a real **branch**, with the default's code in a block of its own,
rather than a select over two already-computed operands. That is what makes
side effects correct: in

```js
let calls = 0;
function bump(v = ++calls) { return v; }
bump(); bump(9); bump();
```

`calls` ends at 2, not 3 — the call that supplied an argument never
evaluates the default. A select would have evaluated `++calls` on all three.
The branch reuses the conditional-expression join machinery of docs/0005
(`snapshotVarStates` / `makeExprJoin` / `bindExprJoinParams`), which is the
same machinery `??` uses, because it is the same shape: a value produced on
one of two paths.

The result of the join is always `dynamic`. The two arms are "whatever the
caller passed" and "whatever the default computed", and nothing proves they
share an unboxed type.

Parameters are bound **left to right**, and each default is evaluated in the
function's own scope after the parameters before it are bound. That is not
an implementation detail: `function later(a, b = a * 2)` reads `a`, and
`function useBase(v = base)` reads the *current* value of a module-level
`base`, because the default is code that runs at call time and not a
constant captured at definition. It follows that a default containing a call
is a call site like any other, which is why the type-inference pass now walks
parameter defaults (decision 9) and why `referencesModuleEnv` now considers
them — a module function whose only mention of a module-level name is inside
a default did not load the module record, and the name resolved to nothing.

## Decision 2 — a rest parameter is not an argument the caller passes

The uniform calling convention is
`bronze_fn_code = (env, this, argc, argv)`, and `FunctionHeader::call` pads a
short call with `undefined` up to the function's `arity`. A rest parameter
does not fit that: its value is *derived from* the arguments rather than
being one of them.

The rule adopted is that **arity is the number of non-rest source
parameters**. `function count(first, ...rest)` has arity 1; `count(1, 2, 3)`
passes three arguments, `argc` is 3, and nothing is padded. `count()` passes
none, is padded to one `undefined` for `first`, and `rest` is `[]`.

The rest array is built in the two places that can see the real count:

- The **call wrapper**, on the uniform path. It is the only generated code
  that sees `argc`, so it calls `bronze_rest_args(argc, argv, first)` and
  passes the array as the entry's last parameter.
- The **direct call site**, where the argument count is a compile-time fact.
  It builds the array with `create.array 0` and `array.append` per leftover
  argument.

Two consequences worth stating because both were bugs first:

`create.array n` makes an array of *n* `undefined` elements, not an array
with capacity for *n*. A container built by appending must start at zero;
starting it at the leftover count gave `count(1, 2, 3)` a `rest.length` of 4.

The wrapper **allocates**, and it does so before the entry's prologue has
rooted anything. docs/0006's contract — "the callee stores its parameters
into its own root frame before it can allocate" — therefore does not cover
it, and `env` and `this` arrive in registers as raw pointers. Under
`BRONZE_GC_STRESS` the collection inside `bronze_rest_args` moved the
instance and left `this` pointing into from-space, so `this.items = items` in
a rest constructor crashed. The wrapper now takes a **two-slot root frame of
its own** around that one call, and builds the rest array *first* so that no
other loaded value can be stale. `argv` needs no protection: it points into
the caller's frame, which the collector updates in place.

## Decision 3 — a spread is a walk that feeds a container, sharing for-of's walk

`...x` in an argument list, an array literal or an object literal is the same
operation: read every element of `x` and put it somewhere. bronze has no
iterator protocol (docs/0012 decision 2), so an array spread is the same
index walk `for-of` performs — `iter.length` / `iter.at` / `iter.advance` —
over the same three kinds of value. That is not a shortcut: it is what makes
`[...s]` and `for (const c of s)` visit exactly the same elements, including
stepping a string by **code point**, so a surrogate pair is one element.
`[..."a😀b"].length` is 3 where `"a😀b".length` is 4.

Where a spread appears, the length becomes a runtime fact, so the shape of
the lowering changes rather than growing a special case:

- An array literal with a spread is built by appending (`array.append` for a
  plain element, `array.spread` for a spread) instead of by writing indices.
- A call with a spread lowers to `call.dynamic.spread`, which takes **one**
  argument operand: an array the runtime unpacks into the argument vector.
  A direct `call` cannot express it, because a direct call's operand list is
  its parameter list, so a spread call always takes the uniform path. The
  same holds for `new` (`new.spread`) and for `super(...)`.
- An object literal's spread is `object.spread`, which copies own enumerable
  properties in the docs/0009 order at the position the spread was written.

Three answers that are stated here because they are observable:

**A later key wins, but an existing key keeps its place.**
`{ ...{ a: 1, b: 2 }, b: 3 }` is `{ a: 1, b: 3 }`; `{ b: 3, ...{ a: 1, b: 2 } }`
is `{ b: 2, a: 1 }` — `b` was inserted first and stays first while taking the
new value. This falls out of the copy being an ordinary property write.

**Spreading `null` or `undefined` into an object is a no-op**, per
CopyDataProperties (ECMA-262 7.3.25), not an error — `{ ...maybeOptions }`
depends on it. An array source copies its indices as the string keys they
are. Any *other* non-object source is a named hard error rather than the
spec's silent `{}`: bronze has no wrapper objects to read index properties
off a primitive, and a quiet empty object is the shape of bug that hides
longest.

**A spread copy is fresh but shallow.** `[...src]` is a new array, so pushing
to it leaves `src.length` alone; an object that was copied into it is the
same object, so mutating it is visible through the original.

## Decision 4 — a pattern is an ordered read, checked once up front

An array pattern reads by position and an object pattern by key. Neither
consults an iterator, for the same reason for-of does not. So
`const [a, b] = xs` is `iter.at` at a cursor chained through `iter.advance`
— chained rather than incremented, because a string's cursor moves by two for
a surrogate pair — and `const { x } = o` is the ordinary `prop.get` that
`o.x` would emit, computed keys taking the `elem.get` path exactly as they do
in an object literal.

Before any element is read, the source goes through **one** `pattern.check`.
It exists for two reasons:

- Destructuring `null` or `undefined` is a TypeError in ECMA-262, raised here
  rather than left as a set of quietly `undefined` bindings — the wrong answer
  given silently, which is what CLAUDE.md forbids. It was a fatal until
  docs/0020 gave the runtime a way to raise a catchable one.
- Checking up front is what lets the diagnostic name **the construct that
  asked**. Without it, `const [a] = 5` would be reported by the shared
  iterator helpers as a for-of error, which sends the reader to the wrong
  line of their program.

Array destructuring of anything that is not an array, a string or a typed
array is therefore
`array destructuring of a value that is not an array, string or typed array`
— never an empty binding.

Evaluation is left to right and observable, and the whole right-hand side is
evaluated before any target is written. That is exactly what makes
`[a, b] = [b, a]` a swap: the array is built first, and every read comes from
that array, so no target write can be seen by a later read. The three-way
rotation `[p, q, r] = [r, p, q]` pins the same claim harder.

An **elision** — `[, x]` — is a named hard error in both a pattern and an
array literal. It is the one form whose meaning is carried entirely by absent
text, and bronze has no sparse arrays for the literal spelling of it either;
supporting one and not the other is how the two would drift apart.

## Decision 5 — the cover grammar is refined in the parser, not in lowering

`[a, b] = pair` parses as an ArrayLiteral, because nothing before the `=`
distinguishes a pattern from a literal. ECMA-262 13.15.5 answers this with a
refinement, and bronze performs it **in the parser**, at the `=`, producing a
`DestructuringAssign` node that holds a real `BindingPattern`.

This retires docs/0016's rationale for putting the error in lowering ("the
parser cannot see it"). The parser can see it: it owns the nodes and can move
them. Doing it here means lowering never has to ask whether an `ArrayLit` on
the left of an `=` is a literal, and the assignment form and the declaration
form share one pattern walker instead of two.

`{ x = 1 }` (a CoverInitializedName) is the other half. It is legal *only* as
a pattern, so it parses — marked `coverInitialized` — and becomes a pattern
element with a default when the `=` arrives. Reaching lowering means no `=`
followed, and that is where the error lives, because lowering is the first
pass that knows.

A member expression as an assignment target — `[o.x] = y` — is a named hard
error: `unsupported construct: a destructuring assignment target that is not
a name or a nested pattern`. It is legal JavaScript and deliberately not
built; naming it is the difference between a missing feature and a wrong
answer.

A pattern parameter has no name of its own, which several passes had to be
taught: it takes an IL parameter named `__patternN` for readability, the
names it binds are what a closure can capture, and those names are what the
scope declares.

## Decision 6 — a for-of head with a pattern binds all of its names per iteration

`for (const [k, v] of pairs)` binds `k` and `v` in the *body's* scope, once
per iteration — the same rule the single-name form gets, and the reason a
closure made inside the loop captures that iteration's values (docs/0012
decision 2). The only change is that the scope-entry hook now takes a *list*
of names written outside the body that belong to it, rather than one name.

## Decision 7 — a derived class with no constructor forwards exactly

ECMA-262 15.7.14 gives it `constructor(...args) { super(...args); }`, and the
parser now synthesizes precisely that: a rest parameter and a `SuperCall`
whose single argument is a spread. It is not an approximation with a fixed
arity — `class CountChild extends Count {}` where `Count` counts its rest
arguments reports 3 for `new CountChild(1, 2, 3)` and 0 for
`new CountChild()`. Chains of implicit constructors forward through every
level.

This is why the class case could not be split into a later chunk: without
rest and spread the forwarding would have had to guess an argument count,
and a guess here is a silently wrong program.

## Decision 8 — what is deliberately not built, and named

Each of these is a named hard error, not a silent acceptance:

- **Elisions** in an array pattern or an array literal (decision 4).
- **Member or index assignment targets** inside a destructuring assignment
  (decision 5).
- **`console.log(...args)`**. `print` takes its arguments as operands, one
  per value, and a spread's length is not known at that point. Naming it
  beats printing the array.
- **`Object.keys(...args)`**, for the same reason its arity is checked.
- **Object spread of a primitive** other than `null` or `undefined`
  (decision 3).
- **`new Float32Array(...args)` / `new ArrayBuffer(...args)`**, whose single
  argument is read at compile time.

## Decision 9 — a non-simple parameter list is not a typed signature

Inference specializes a module function's signature by joining over every
call site (docs/0010 decision 5). That is sound only while there is a
one-to-one correspondence between an argument and a parameter, and every one
of this chunk's parameter forms breaks it: a default means the bound value
may be something no caller passed, a rest parameter means several arguments
become one value, and a pattern means the bound names are pieces of an
argument rather than the argument.

So a function with any default, rest or pattern parameter is **not
direct-callable** and keeps the uniform dynamic convention. This costs
nothing that was previously working and removes a whole class of unsound
proof.

Two smaller consequences in the same pass:

- Parameter defaults and pattern computed keys are **walked** by the flow
  analysis. They are code that runs in the function and appears nowhere in
  its body, so leaving them out hid their call sites from the widening pass
  — which is not a missed optimization but an unsound proof: a callee's
  parameter could be inferred `number` while a string reached it from a
  default.
- Every name a pattern binds is `dynamic`, including in a destructuring
  *assignment*, where the write must widen a name that was previously proven
  numeric.

## Bugs this chunk uncovered in existing code

Two, both pre-existing and both GC rooting:

**`bronze_string_concat` used its second operand's raw bits across an
allocation.** It converted `a` to a string — which allocates for a number —
and only then read `b`, whose raw pointer had been left stale by the
collection. Under `BRONZE_GC_STRESS`, `a.length + ":"` produced `"1"`: the
`":"` had moved and the stale pointer read an empty string. Both operands are
now rooted before either conversion runs. Nothing in the suite had a
number-then-string concatenation where the string was allocated close enough
to the collection to expose it.

**`Object.keys` re-encoded every key as UTF-8.** It copied arena-interned
shape keys with `createFromUTF8(latin1View(name))` unconditionally, so any
property name outside Latin-1 came back as mojibake. The copy now preserves
the encoding (`rtCopyKeyToHeap`), and object spread and object rest go
through the same helper rather than growing a second copy of the mistake.

The rest-parameter wrapper's missing root frame (decision 2) was a bug in
*new* code, but it is worth recording next to these: all three were found by
the same mechanism, `oracle-gc-stress` running every case with collection
forced on every allocation (docs/0006 decision 5).

## Diagnostics added

Parser:

- `a rest parameter must be a plain name`
- `a rest parameter may not have a default value`
- `a rest parameter must be the last parameter`
- `a rest element may not have a default value`
- `a rest element must be the last element of an array pattern`
- `a rest property may not have a default value`
- `a rest property must be the last element of an object pattern`
- `a destructuring declaration requires an initializer`
- `a destructuring pattern may only be the target of '='`
- `unsupported construct: an elision (a hole) in an array pattern`
- `unsupported construct: an elision (a hole) in an array literal`
- `unsupported construct: a destructuring assignment target that is not a
  name or a nested pattern`
- `'...' is only allowed in an argument list, an array literal, an object
  literal, or a binding pattern` — replaces the old `unsupported construct:
  spread`
- `a 'try' requires a 'catch' or a 'finally'` — see below
- plus `unterminated array pattern` / `unterminated object pattern` and the
  expectation messages for the pattern grammar's punctuation.

Lowering:

- `a shorthand property may not have an initializer outside a destructuring
  pattern` — decision 5, replacing docs/0016's parse-time message.
- `unsupported construct: '...' outside an argument list, an array literal
  or an object literal`
- `unsupported construct: a spread argument to console.log`

Runtime:

- `array destructuring of null or undefined`
- `object destructuring of null or undefined`
- `array destructuring of a value that is not an array, string or typed
  array`
- `spread of a value that is not an array, string or typed array`
- `object spread of a value that is not a plain object, an array, null or
  undefined`
- `object rest from a value that is not a plain object`

Retired: `unsupported construct: default parameter value`, `rest parameter`,
`destructuring parameter`, `destructuring declaration`, `destructuring
assignment`, `spread`, `destructuring assignment pattern`, and
`a derived class with no constructor is not supported`.

## An unrelated parser gap fixed on the way

`parseTry` never consumed a `finally` block, and `finally` was not a keyword
at all — it lexed as an identifier. A `try { } finally { }` was therefore
reported as `expected ';' after expression statement, got '{'`, which names
punctuation instead of the construct lowering already has a name for. The
keyword is added, the block is consumed, and a `try` with neither handler is
now `a 'try' requires a 'catch' or a 'finally'`. try/catch/throw itself
remains unimplemented and is one of the three re-seeded blocked cases.
