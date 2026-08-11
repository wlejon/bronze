# 0016 — Names, declarations, and literals

Status: implemented. Part of phase 4 of docs/0001.

Four things in this chunk were reported to the user as `undefined variable`
or `expected ...` when the source was ordinary, correct JavaScript, and one
was reported as nothing at all — it compiled and printed a plausible wrong
number. The name-resolution pair came first because the first of them,

```js
let x = 5;
function f() { return x; }
```

is in essentially every real file, and it did not compile.

The rest of the chunk is the declaration and literal grammar that had grown
around the parts already built: several declarators in one `let`, the empty
statement, the numeric literal forms with a radix prefix or a separator,
object shorthand and computed keys, and `console.log` with more than one
argument.

## Decision 1 — the module scope's record is a singleton, published to a global

A top-level `function` declaration lowers to a *module function*: a real IL
function with a name, which a caller reaches with `call @f`. Closures reach
their captured variables through an environment record passed as the
synthetic first parameter `__env` (docs/0007), and the verifier enforces
that a direct `call` never targets a function that needs one. A module
function therefore had no way to name a module-level `let`, and the capture
scan never looked inside one, so every reference to `x` above was a
compile error.

The obvious repair — desugar top-level `function` declarations into closures
— would have been the wrong one, and not marginally. It is transitive: a
module function that calls a converted one must itself be reachable from a
context holding the environment, so it converts too, and the cascade stops
only when almost nothing is directly callable. docs/0002's log attributes
the 11x on `fib` to typed direct calls. Paying that for name resolution
would trade the project's central performance claim for a scoping fix.

The observation that avoids the trade is that **the module scope is entered
exactly once**. Every other environment record is one per activation and has
to be threaded through the call, because there can be many of them live at
the same time and a callee must be told which one. There is only ever one
module record per process. So it does not need to be threaded: `main`
creates it, publishes it with `module.env.set`, and any module function that
needs it loads it at entry with `module.env.get`. Direct callability is
untouched — the signature does not change, and the load is an ordinary
instruction in the body.

Three consequences that the implementation has to honour:

- The **layout is fixed before any body is lowered**. `planModuleEnv` runs
  ahead of the function pass and installs the module scope permanently at
  `envScopes_[0]`, so the `(depth, index)` pair computed inside a module
  function and the one computed inside a closure nested three deep both
  count hops to the same record. Allocating slots lazily as bodies were
  lowered would make the depth of a name depend on which function was
  lowered first.
- The global is a **GC root**. `g_moduleEnv` in `rt_state.cpp` is visited by
  the same root source that already visits the runtime's other heap-holding
  globals (docs/0006). A module record the collector cannot see is a
  use-after-free that a normal run would almost never show; `oracle-gc-stress`,
  which collects at every allocation, would show it immediately, and does not.
- `main` publishes **before** it runs any statement, and `main` is the entry
  point, so no module function can observe the global unset.

Whether a given module function emits the load is decided by
`referencesModuleEnv`, an over-approximation: it asks whether the body
mentions any name the record holds, descending into nested functions, and
loads if so. The cost of guessing wrong in that direction is one load in a
function that did not need it. The cost of guessing wrong in the other
direction is an unresolved name.

**Not covered:** the temporal dead zone. Calling a top-level function above
a `let` that the function reads yields `undefined` where ECMA-262 requires a
ReferenceError. bronze has no `throw`, so there is nothing to raise; this
belongs with exceptions, not here.

## Decision 2 — "what the record holds" and "what a closure captures" are different questions

Making decision 1 work meant scanning the whole module for captured names,
and the first version of that reused `capturedNames_`. The ratchet caught it
within the minute: `typed_direct_calls` began failing with `unsupported
construct: closure capturing the for-loop binding 'i'`, a diagnostic about a
top-level `for (let i = ...)` that no closure captures.

The two sets answer different questions:

- the module record's layout must be derived from **every** top-level
  function body, because that is exactly what decision 1 exists to serve;
- `capturedNames_` asks which of the *current scope's block-level* bindings
  a closure can reach, and it drives the for-header capture diagnostic
  (docs/0007 decision 2), which is a hard error.

Widened to the first set, the second question answers yes for a top-level
`i` merely because some unrelated function also declares an `i`. They are
kept apart: `planModuleEnv` uses a local set for slot allocation, and
`openModuleEnv` sets `capturedNames_` to the original narrow set. A
block-level binding is never a module declaration, so it can never be in the
record, and separating them loses nothing.

## Decision 3 — an update expression is a read and a write, and must resolve like one

```js
function outer() { let n = 0; return () => ++n; }
```

failed with `undefined variable: n`, while `n = n + 1` and `n += 1` in the
same position worked. The assignment path in `lower_expr.cpp` looks a name
up in `activeVarMap_` and, when it is not there, falls back to
`findEnclosingEnvVar` plus `emitEnvGet`/`emitEnvSet`. The update path had
its own, shorter lookup that consulted `activeVarMap_` and errored.

`++n` is `n = ToNumeric(n) + 1` with the old value as the result in postfix
position. Two paths that resolve the same name in the same scope by
different means are one edit away from disagreeing, and they had already
disagreed. The update path now performs the same two-case resolution and
routes its read and its write through the same helpers.

The neighbouring collectors were checked for the same asymmetry rather than
assumed correct: `assigned_set.cpp` already treated an update as a write,
and `queries.cpp`'s capture visitor already reached the operand through the
generic unary walk.

## Decision 4 — `lower()` resets scope state between functions, and the leak is pinned

Underneath the two bugs above was a third with no diagnostic at all:

```js
function f(p) { let secret = 42; return p + secret; }
console.log(secret);
```

compiled, and printed `43`. `lower()` cleared its scope tables at the start
of each function body but not before lowering `main`, so the bindings of the
*last* module function were still live at module level. `secret` resolved,
carrying the IL value id its binding had inside `f` — which in `main` names
an unrelated instruction's result. A wrong answer invented out of another
function's SSA numbering, with no error and nothing in the output to suggest
one.

The same leak had a second, opposite face: a top-level `let acc` after any
function with a local `acc` was rejected as `redeclaration of variable 'acc'
in same scope`, refusing a correct program.

`main` now resets the same state every other function body resets. The
restructure in decision 1 would have fixed this as a side effect, which is
precisely why it is pinned on its own: `tests/oracle/cases/scope_binding_isolation.js`
covers the wrong-answer face with ordinary colliding names, and a
`tests/lower` case pins the compile-error face — `console.log(secret)` must
be `undefined variable: secret`. A case named for the leak is what stops it
returning the next time `lower()` is restructured.

## Decision 5 — leaving a scope restores the shadowed binding, it does not erase it

```js
let x = 1;
{ let x = 10; }
return x;
```

was `undefined variable: x`. `exitScope` erased its scope's names from
`activeVarMap_`, and an inner declaration had overwritten the outer entry on
the way in, so the outer binding was erased with it. A `VarBinding` now
records the binding index it shadowed, and `exitScope` puts that back —
erasing only when there was nothing underneath. The map behaves as the stack
of scopes it models rather than as a flat set of live names.

## Decision 6 — a BindingList is several declarations, not a block

`let a = 1, b = 2, c` is one `LexicalDeclaration` containing a `BindingList`
(ECMA-262 14.3.1), and each binding is instantiated separately into the
running execution context's record. `parseVarDecl` returned a single
statement, so the natural repair is to let it return several — it takes an
output vector, and every statement production now appends rather than
returns.

It would have been less work to wrap the declarators in a `BlockStmt`, and
it would have been wrong: a block is a scope. `let a = 1, b = 2;` followed
by a use of `a` would stop compiling, because `a` would have gone out of
scope at the end of the wrapper. The same reasoning makes `ForStmt::init` a
*list* of statements rather than a block — the header's bindings belong to
the loop's own scope and are visible to the condition, the update and the
body, and `for (let i = 0, j = n; i < j; i++)` needs both of them there.
docs/0014 decision 4 already routes the header's semicolons around ASI, and
that split is what lets one `parseVarDecl` serve both positions.

## Decision 7 — the empty statement consumes its token and contributes no node

`;` on its own is an EmptyStatement (ECMA-262 14.4). It has no runtime
effect, so it produces no AST node — but it still *consumes* the semicolon,
which is what keeps the "every parser consumes all input or errors" rule
true and keeps the caller's statement loop making progress. Returning
success without consuming would loop forever; producing an empty node would
put something in the AST that no phase should have to skip.

Its second use is as a loop body: `while (advance()) ;` is a complete
statement whose body does nothing, and the parser reaches it through the
same production as any other single-statement body.

## Decision 8 — the lexer finds a numeric literal's end; the parser decides what it denotes

`0xFF` lexed as `0` followed by an identifier `xFF`. The two halves of the
repair are deliberately not in the same place.

The **lexer** scans to the end of the literal and no further. It recognises
the three radix prefixes, scans all three with the hex digit set (a superset
of the other two), and consumes a separator wherever one appears — including
where a separator is illegal, so `1_` is one malformed token rather than a
number and an identifier. It makes exactly two lookahead judgements, both
about a boundary rather than a value: a `.` joins the literal only when a
digit or separator follows (so `1.foo` stays a property access), and an `e`
begins an exponent only when a digit, sign, or separator follows (so `1e` is
still two tokens, and `1e_5` is one token the parser can name the fault in).

The **parser** decides what the text means, which is where every diagnosis
lives. Legacy octal is the reason the split matters: `017` is 15 read as
octal and 17 read as decimal, and ECMA-262 makes it a strict-mode
SyntaxError precisely because neither reading may be assumed. Treating it as
decimal is a silent wrong answer of the kind docs/0000 puts at the top of
the list, so bronze names it and suggests both spellings. `08` — a
NonOctalDecimalIntegerLiteral — is the same production's other half and gets
the same error.

Separator placement is checked against the original text, before any
stripping, because the rule is about which characters a `_` neighbours: not
leading, not trailing, not doubled, and not adjacent to the radix prefix or
the decimal point. All of them are errors. Accepting them quietly would mean
`1_._5` and `1.5` compile to the same program while only one of them is JS.

Exponent notation and a leading-dot decimal (`1e3`, `.5`) came in with this
decision rather than after it. They are the same question — a literal form
the lexer did not have — and both were hard errors before.

## Decision 9 — a computed key is a runtime ToPropertyKey, so it takes the element path

`{ [k]: v }` was `expected identifier or string literal for property key`.
`ObjectProp` gained a key *expression*; the written `key` string and the
`keyExpr` are never both meaningful, and a computed key dumps under its own
head (`prop-computed`) because two constructs that lower differently must
not dump identically — docs/0012 decision 3.

Evaluation order is the part that is easy to get wrong and impossible to see
afterwards. ECMA-262 13.2.5.5 evaluates each PropertyDefinition in source
order and, within one, the key before the value. Both are pinned by a case
whose keys and values are calls that append to a log.

Lowering a computed key emits `ElemSet` rather than `PropSet`, which routes
it to `rt_prop.cpp`, where the key is ToPropertyKey'd and handed to the same
`setProp`/`getProp` the named path uses. This replaces a named error with an
implementation on a path that already existed for arrays, rather than
inventing a second property protocol. ToString of a number is the runtime's
`formatJsNumber`, which is also why a *numeric* key in source (`{ 1: 'a' }`)
is still a named parse error rather than being spelled out by the parser:
its name comes from the runtime's formatter, and the parser may not
reimplement it. `{ [1]: 'a' }` is the spelling that works, and it is the
same property.

Inference declines shape classes for a literal with any computed key. A
shape interned over the written keys alone would be a claim about a layout
the runtime never builds, and the inline property caches rest on that claim
being true.

Shorthand `{ x }` is the simpler half: the key is the identifier's text and
the value is that same identifier evaluated in place, built from one token
so the two cannot disagree about which binding is meant. The two other
things an identifier in key position can begin are now named rather than
reported as a missing `:` — a method shorthand `{ m() {} }`, which is a
`super` and home-object question, and a CoverInitializedName `{ x = 1 }`,
which is only ever a destructuring pattern.

## Decision 10 — one formatter; argument separation belongs to the caller

`console.log` accepted exactly one argument and named a hard error for any
other count. node's `console.log` inspects each argument and joins them with
a single space.

`Print` takes N operands and the runtime gained `bronze_print_values(argc,
argv)`. What it did *not* gain is a second formatter: the newline was split
out of the existing single-value path so that both entry points share one
`writeValue`, which is still the docs/0013 inspect format. This is the same
argument that put `Math.pow` and `**` on one `rtExponentiate` — two
formatters would drift, and every `.expected` file in the suite is a byte
comparison against one of them.

Two edges follow from the join rather than from the formatter and are pinned
as such: `console.log()` with no arguments prints an empty line, and the
separator is a single space regardless of what the neighbouring values
inspect to. The argv region for the call is planned into the caller's GC
root frame the same way a dynamic call's is (docs/0006), because inspecting
one argument can allocate while the others are still only reachable from it.

## Decision 11 — the AST dump prints the shortest text that round-trips

Found while pinning decision 8: the dump formatted numbers with
`ostringstream <<`, whose default precision is six significant digits.
`123.4567` dumped as `123.457`, so two literals that are not the same number
could dump identically — in the one artefact the parser's tests compare, in
the chunk whose whole subject is which number a literal denotes. It is also
locale-sensitive for the decimal point, and `bronze parse` prints it to
stdout, which makes it an output path and the determinism rule (docs/0001)
applies to it directly.

It now uses `std::to_chars`, like the IL printer. That is exact and
round-trips, and it is not the same as JS `Number::toString` — `1000000`
dumps as `1e+06` because that is the shorter of the two forms that
round-trip. The dump is a debugging artefact for the AST, not a rendering of
a JS value; the runtime's `formatJsNumber` remains the only place that
answers what a program *prints*, and it stays in the runtime.

## Named diagnostics

- `legacy octal literal '<text>': a numeric literal may not start with '0'
  followed by a digit (write 0o<digits> for octal, or <digits> for decimal)`
  — decision 8, and the one place in this chunk where a silent guess would
  have produced a plausible wrong number.
- `numeric separator '_' must appear between two digits, in '<text>'` —
  covers leading, trailing, doubled, post-prefix and around-the-point
  placements with one message, because they are one rule.
- `invalid digit '<c>' in the <hexadecimal|octal|binary> literal '<text>'`
- `the <hexadecimal|octal|binary> literal '<text>' has no digits after its
  prefix`
- `malformed numeric literal '<text>'` — the residual case, when the decimal
  text is not something `from_chars` reads in full.
- `empty numeric literal`
- `expected a property key: an identifier, a string literal, or a computed
  '[expr]'` — replaces "expected identifier or string literal for property
  key", which named neither of the two forms that now exist.
- `unsupported construct: object literal method shorthand` — decision 9.
- `unsupported construct: destructuring assignment pattern ('{ x = ... }' is
  only ever a pattern, never an object literal)` — decision 9.
- `unsupported construct: spread` — reached from key position, where the old
  message claimed a missing identifier.
- `computed index access is only supported on arrays, plain objects and
  Float32Array` (and the matching write) — reworded, since plain objects now
  reach this path by decision 9.
