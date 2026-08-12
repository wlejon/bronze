# 0027 — An unresolvable name is not a compile error, and `arguments`

Status: implemented. Part of phase 4 of docs/0001, and the chunk that turns
"the three.js closure parses 28/28" into "the three.js closure builds".

The blocker was not a missing feature. It was a policy: bronze refused any
free identifier that was not on its provided-globals list, and two of the most
ordinary idioms in real-world JavaScript are exactly that.

```js
// three.js scenes/Scene.js:22 — the universal feature-detection idiom
if ( typeof __THREE_DEVTOOLS__ !== 'undefined' ) { ... }

// three.js utils.js:67 — inside a function a headless run never calls
function createElementNS( name ) { return document.createElementNS( ns, name ); }
```

Both were `error: undefined variable: X`. Both are legal JavaScript, and the
second one is legal *and* harmless: the function is compiled, never called,
and its body never evaluated.

## Decision 1 — the provable/unprovable line, and why this is not going soft

"Hard errors over silent fallbacks" (docs/0001 decision 8) is a founding rule
and it is why the old behaviour existed. This decision does not weaken it. It
fixes a hard error that was firing in the wrong place, on correct programs.

The line, stated so it can be applied to the next case rather than argued
again:

> **What bronze can prove, it diagnoses now. What only the running environment
> can answer, it answers the way the spec answers it — at run time — and warns
> about at compile time.**

Both halves are load-bearing, and the pair that shows the line is:

- `console.table` is a **compile error, by name**, in dead code and under
  `typeof` alike. bronze *knows* `console` exists and knows exactly which
  members it has, so an unknown member is a fact bronze holds: it is a typo,
  and diagnosing it now is the whole value of docs/0011 decision 3. That
  behaviour is unchanged by this chunk and must stay unchanged.
- `__THREE_DEVTOOLS__` and `document` are **not** things bronze knows anything
  about. There is no list they are absent from — there is no list. Answering
  "that name does not exist" at compile time is not a proof, it is a guess
  about a runtime environment this compilation cannot see.

So the rule for an identifier that falls off the whole resolution ladder is
ECMA-262's own:

- **`typeof <unresolvable>` is the string `"undefined"`.** 13.5.3 step 1: "If
  val is a Reference Record and IsUnresolvableReference(val) is true, return
  `"undefined"`." No error, no throw, and — see below — no warning either.
- **Every other evaluation of one throws `ReferenceError: X is not defined`,
  where it is evaluated.** 6.2.5.5 GetValue step 2. A function that names
  `document` and is never called throws nothing, because its body is never
  evaluated. This is what makes `utils.js` build.
- **A compile-time warning names each unresolved identifier, once per name.**
  The build still tells you `document` is unresolved. It just does not refuse
  the program over it.

Nothing is lost in loudness. A compile error became *a runtime throw the spec
defines plus a compile warning*, and the throw is louder than the old error in
one respect that matters: it names the moment the program actually depended on
the name, which a compile error never did.

The **warning is per NAME, not per mention**. `document` appears eleven times
in three.js's `utils.js`; eleven identical warnings is a diagnostic nobody
reads. It is emitted from one place (`Lowerer::emitReferenceError`) against a
per-module set, and lowering order is deterministic, so the warning stream is
too.

### Why bare `typeof` gets no warning at all

Every other unresolved reference is a question about the environment that
bronze cannot answer. `typeof x` is not that question — it is the question
"*is* there an `x`?", and the language guarantees an answer. Warning about it
would put a diagnostic on the one idiom whose entire purpose is to handle a
name's absence correctly, on every file of every real library. So the
exemption is total: no error, no warning, and not even a `typeof` instruction
— lowering folds it to the constant string.

The exemption is for an unresolvable **Reference**, and it does not spread.
`typeof x.y` where `x` is unresolvable still evaluates `x`, so it still
throws; that is pinned.

### Where it does NOT apply: a name bronze declared and failed to bind

`const f = function rec(n) { ... rec(n - 1) ... }` is a known bronze bug
(docs/0023): 15.2.5 puts a named function expression's own name in a scope of
the function's own, and bronze does not bind it. `rec` is therefore a
*declared* name bronze fails to resolve — not an unresolvable one — and if it
fell into decision 1 it would compile to a throw that a program could even
`catch`, hiding a compiler limitation behind a language error.

So lowering keeps a stack of the function expressions whose bodies it is
inside, and a failure to resolve one of those names is a hard error that says
what it is:

```
error: unsupported construct: a named function expression cannot refer to
itself by name ('rec')
```

The bug is unchanged; only its diagnostic improved. This is the general shape
of the guard the line above needs: when bronze *does* hold a fact about a
name, decision 1 must not swallow it.

### The mechanism

One IL instruction, `ref.error "<name>"`, and one ABI helper,
`bronze_reference_error(keyIndex)`, which raises through the same
`rtThrowError` path as every other spec'd error (docs/0020 decision 6). It is
not a terminator: it is an ordinary helper call, `il::canThrow` admits it, and
the backend's existing exception test after it takes the block's handler. So
the error is catchable, `e instanceof ReferenceError` and `e instanceof Error`
are both true, and nothing about the unwind path is new.

`ReferenceError` became the fifth member of the `Error` family and the
sixteenth provided global, for the reason `TypeError` is one (docs/0020
decision 7): a program that catches what bronze raises must be able to name
its class.

The order of evaluation is the spec's, not the convenient one:

- a **read** throws where it is read;
- a **compound** assignment (`missing += f()`) throws at its GetValue, before
  the right side runs (13.15.4 step 1);
- a **simple** assignment (`missing = f()`) evaluates the right side first and
  throws at PutValue (13.15.2), so the call's side effects still happen.

That a simple assignment throws at all rather than creating a global is
13.15.2 step 6's strict-mode answer. bronze does not have a strict mode
(`cases/blocked/strict_mode.js`), and it does not need one here: sloppy mode's
implicit global was never available, because bronze has no global object to
create a binding on (docs/0011 decision 1). One answer in every position is
the only answer bronze can give.

### What inference may believe

An unresolved name is `dynamic` — the type lattice's designed sound fallback
for "nothing was observed here" (docs/0010 decision 2), and the answer
`FlowAnalyzer::lookup` already gave a name it never saw. What needed saying
out loud, and is now pinned in `tests/types`, is the second half: the code
*around* it stays analysable. A `typeof` guard is an ordinary condition, both
its arms are live, and a neighbouring binding assigned from literals is still
proven a number. Treating the guarded arm as unreachable would be the silent
wrong answer this whole chunk exists to avoid.

## Decision 2 — the provided-globals list stays closed

Decision 1 changes what happens when the closed-list check *misses*. It does
not open the list, and it must not be read as licence to.

`isProvidedGlobal` is still the whole set of names bronze resolves, still
checked at compile time, and still the last rung of the ladder so a local
declaration shadows any of them with no special case. A name on it becomes
`global.get`; a name off it becomes `ref.error`. Neither becomes a runtime
lookup that answers `undefined` — which is the thing docs/0011 decision 1
refused and still refuses.

This is also why **no browser global was added**. `document`, `window`,
`CustomEvent` and `fetch` are precisely what decision 1 makes unnecessary:
`utils.js` builds, and its browser-only functions throw only if called, which
headlessly nothing does. Adding a stub for any of them would be the silent
fallback the project forbids, with the extra cost that a program could then
feature-test its way into a code path bronze cannot run.

## Decision 3 — `arguments` is a binding, not a keyword, and it is unmapped

`Object3D.add` is on the critical path of the target application:

```js
add( object ) {
    if ( arguments.length > 1 ) {
        for ( let i = 0; i < arguments.length; i ++ ) this.add( arguments[ i ] );
        return this;
    }
    ...
}
```

**It is an ordinary binding named `arguments`.** That is the whole design.
`arguments` is an Identifier, not a keyword, so a parameter or a declaration of
that name can shadow it — and 10.2.11 step 22 says so: the object is built only
when the name is not already bound. Making it a binding means the shadowing
rule needs no code at all, and, more importantly, means an arrow reaches the
enclosing function's `arguments` through the machinery docs/0012 decision 3
already built for `this`: capture analysis sees the name mentioned in a nested
arrow, `enterFunctionEnv` gives it an environment slot, the prologue copies the
synthetic parameter into it, and an `arguments` in the arrow body resolves to
an `env.get` walking out as many levels as needed. There is no second
mechanism. `ast::usesArguments` is `ast::usesThis` with a different thing to
find and the same boundary: descend into arrows, stop at every other function.

**It arrives as a synthetic parameter, built by the call wrapper.** The object
is every argument the caller *actually passed*, and the only code that can see
that is the wrapper — the same fact that made a rest parameter's array the
wrapper's job (docs/0017 decision 2). So `il::Function` grows `needsArguments`,
the parameter list becomes `[__env?] [__this?] [__arguments?] source...`, and
`bronze_arguments_object(argc, argv)` is called under the same two-slot root
frame `emitRestArgs` already needed, because it is the same hazard: an
allocation before the entry's prologue has rooted `env` and `this`.

A function that needs one is therefore **not a direct-call target**, the same
exclusion `needsEnv` carries and for a related reason.

**It declares arity 0, and that is not a detail.** `FunctionHeader::call` pads
a short call up to the declared arity with `undefined`. Padding would make
`f(1)` indistinguishable from `f(1, undefined)`, where the language says
`arguments.length` is 1 and 2 — so a function owning an arguments object opts
out of padding (`il::Function::adaptArity`) and its wrapper reads argv through
`bronze_arg_at`, which answers `undefined` past the end. Every other wrapper's
unguarded load is still correct, because for every other function the padding
did happen. This was a live wrong answer during the chunk: `two(1)` reported
`arguments.length` of 2.

**It is UNMAPPED, by name.** ECMA-262 has two arguments objects: the mapped one
(10.2.11 step 21, non-strict function with a simple parameter list), where
writing `arguments[0]` writes the parameter, and the unmapped one
(CreateUnmappedArgumentsObject), where it does not. bronze always builds the
unmapped one. The mapped object is a silent aliasing trap, three.js never
relies on it, and implementing it would mean an exotic object whose index
properties are accessors over parameter slots. `arguments_object.js` pins the
unmapped answer explicitly (`original/written`, not `written/written`) so the
divergence is a committed byte rather than a footnote.

### The divergence that is a real one, and is not hidden

The object bronze builds is an ordinary **array**. `length`, indexing and
iteration are therefore exactly right and free — including `for-of`, since an
array already has the fast iteration kind of docs/0021 decision 2. Three
observable things are wrong:

- `Array.isArray(arguments)` is `true`; a spec engine says `false`.
- `arguments instanceof Array` is `true`; a spec engine says `false`.
- `console.log(arguments)` prints `[ 1, 2 ]` where node prints
  `[Arguments] { '0': 1, '1': 2 }`.

The alternative is a new heap object kind — a new `flags` value, which every
`flags` dispatch in the runtime would have to learn: property get and set,
element get and set, enumeration, `inspect`, `iter.open`, and the collector's
payload scan. That is a real object kind's worth of work to buy three
answers, none of which three.js asks. It is recorded here, and pinned in
`tests/runtime`, so it is a decision that can be revisited rather than a fact
someone rediscovers.

## Decision 4 — the four global numeric functions are provided globals

`isNaN`, `isFinite`, `parseInt`, `parseFloat` (ECMA-262 19.2.2–19.2.5).
`Ray.js` and `Color.js` are both in the closure and both need them.

They join `isProvidedGlobal` rather than being recognized at the call site, for
the reason `Math` did (docs/0011 decision 1): `arr.map(parseFloat)` is ordinary
JavaScript and a call-site recognition cannot express it.

Two things fall out of the representation and are worth stating because they
are the spec's requirements, met for free:

- `parseInt` and `parseFloat` reuse the code pointers of `Number.parseInt` and
  `Number.parseFloat`, and `bronze_function_singleton` interns by code pointer,
  so `parseInt === Number.parseInt` — which is exactly what 21.1.2.12 and
  21.1.2.13 mean by "the same function object".
- `isNaN` and `isFinite` are **new** functions, not the `Number` statics.
  19.2.2 and 19.2.3 begin with ToNumber; 21.1.2.2 and 21.1.2.3 deliberately do
  not. `isNaN("x")` is true and `Number.isNaN("x")` is false, and that contrast
  is the whole reason both spellings exist. Pinning only one of them would have
  let the wrong function serve both names.

The parsers themselves were already there, and were already right, with one
exception found by deriving the edge table from 19.2.5 rather than from
intuition: step 4 is **ToInt32(radix)**, not a truncation, so `parseInt("10",
Infinity)` is 10 — ToInt32(Infinity) is 0, which selects the default radix.
bronze used `static_cast<int>` of the double, which is undefined behaviour for
an infinity and produced `INT_MIN` here, so the range check rejected it and the
answer was `NaN`. Fixed, and the edge is in the pinned table.

### The bug this cost, and what found it

A wrapper can now build TWO arrays before the entry runs: the `arguments`
object and the rest array. The root frame `emitRestArgs` had protected only
`env` and `this`, so the second allocation moved the first array and left the
wrapper holding a stale pointer — which landed on the array that had just been
allocated over it. `function withRest(a, ...rest)` called as `withRest(1)`
reported `rest.length` of 1 instead of 0: `rest` WAS the arguments array.

The plain oracle run passed. Only `BRONZE_GC_STRESS=1` showed it, which is the
whole argument for that mode existing (docs/0020 decision 1 records the same
lesson about root registration). The helper now takes a list of live values,
and each array already built joins the next one's frame.

## Divergences from node

- `arguments` is an array (decision 3): `Array.isArray`, `instanceof Array`
  and `console.log`'s label all differ. Deliberate, argued above.
- The mapped arguments object does not exist (decision 3). For a module —
  which is strict code, 16.2.1.6.4 — a spec engine agrees; for the sloppy
  script bronze actually compiles, it does not.
- An unresolved name produces a compile-time warning on stderr that node has
  no equivalent of. It is on stderr precisely so that a pinned stdout is
  unaffected by it (docs/0026 decision 6 made the same call for
  `console.warn`).

## Named diagnostics this chunk introduces

- `warning: unresolved name '<name>': a ReferenceError if it is evaluated
  (bare `typeof <name>` is safe)` — decision 1. Once per name, per module.
- `error: unsupported construct: a named function expression cannot refer to
  itself by name ('<name>')` — decision 1's guard. Replaces the
  `undefined variable: <name>` this case used to produce by accident.
- `error: cannot assign to '<name>'` — a name that DOES resolve, but not to
  something an assignment or an update can write: a module function, a
  provided global, `undefined`. Previously also reported as
  `undefined variable`, which named the wrong problem.
- `ReferenceError: <name> is not defined` — at run time, catchable.

## Oracle cases

- `unresolved_typeof.js` — the idiom in both directions, two more unresolved
  names, and the resolvable cases for contrast.
- `unresolved_reference.js` — a function that names `document` and is never
  called; the same one called, caught, with `name`, `message` and both
  `instanceof`s; `typeof x.y`; the simple/compound assignment ordering; an
  unresolved name in an argument list; and the program continuing afterwards.
- `arguments_object.js` — the count as passed rather than declared, indexing
  past the end, iteration, `...rest` alongside, the unmapped write, an arrow
  and a doubly-nested arrow reading the enclosing object, a nested ordinary
  function shadowing with its own, a class method, a function expression, and
  a parameter of that name winning.
- `global_numeric_functions.js` — the coercion contrast against the `Number`
  statics, the `parseInt` radix and prefix rules, the `parseFloat` longest-
  literal-prefix rules, both on non-string arguments, the `===` identities,
  and shadowing.

## Files this chunk added, and the seams it cut

- `tests/runtime/arguments_test.cpp` — the object below the compiler, and the
  one place the array divergence is named as an assertion rather than as
  prose.
- `src/lower/lower_unresolved.cpp` — the resolution ladder asked as a
  question (`resolvesName`), the once-per-name warning, and the one
  instruction an unresolvable name lowers to. It exists so that `typeof`'s
  test for "does this resolve?" and the read path's *performance* of the same
  ladder cannot drift apart.
- `tests/lower/lower_{infer,scope,unresolved}_test.cpp` and
  `tests/lower/lower_fixture.h` — `lower_test.cpp` crossed 950 lines while
  this chunk was in it and was split along the seams `src/lower` already
  names. A pure move: same assertions, same text. The fixture header replaces
  three copies of the same two pipeline helpers.
