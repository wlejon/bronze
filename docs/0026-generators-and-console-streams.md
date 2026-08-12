# 0026 — Generators as a desugaring, and the two console methods that must not write to stdout

Status: implemented. Part of phase 4 of docs/0001, and the last parse blocker
before a three.js scene graph compiles.

Two things that look unrelated and arrive together because they are the same
kind of work: both are surface that three.js uses on ordinary code paths,
both are recognized by NAME in the parser, and both were hard errors that
stopped a real file at its first line.

The measurement that shaped both:

- The 28-file closure of a minimal three.js scene-graph application parses
  23/28, and **all five remaining failures are `*[Symbol.iterator]()` in a
  class body** — Vector2, Vector3, Vector4, Euler, Quaternion and Color, the
  whole math core.
- Across three.js 0.160.0 (374 files, 60,829 lines) there are **exactly 6
  generators and 20 `yield`s**, and every one of them has the same shape.
- three.js calls `console.warn` 91 times and `console.error` 62 times, against
  20 calls to `console.log`.

## Decision 1 — a generator is a DESUGARING, restricted to the straight-line subset

Every generator in three.js is this:

```js
*[ Symbol.iterator ]() {
    yield this.x;
    yield this.y;
    yield this.z;
}
```

No loop around a yield. No conditional yield. No `yield*`. No use of a
yield's value. No `return` with a value, no `try`. Six out of six.

So bronze does not build a coroutine, and does not build a state-machine
transform. The parser rewrites the body into an iterator object over a step
index, before the AST leaves the parser:

```js
@@iterator() {
    let step = 0;
    return {
        next: () => {
            if (step === 0) { step = 1; return { value: this.x, done: false }; }
            if (step === 1) { step = 2; return { value: this.y, done: false }; }
            if (step === 2) { step = 3; return { value: this.z, done: false }; }
            return { value: undefined, done: true };
        },
        "@@iterator"() { return this; }
    };
}
```

Three properties of that shape are doing the work, and none of them is an
accident:

- **`next` is an ARROW.** `this` inside a yielded expression is then the
  receiver the method was called on, captured lexically (docs/0012 decision 3)
  — which is the entire reason the three.js generators exist. A method
  shorthand would have bound `this` to the iterator object and every
  `yield this.x` would have read `undefined`.
- **The step lives in the enclosing invocation's environment record**
  (docs/0007), not in `next`'s frame. That is what makes two calls to
  `@@iterator` two independent walks, and what makes a second `for-of` over
  the same object start from the beginning.
- **The iterator is its own iterable** (ECMA-262 27.5.1.2), through an
  ordinary method that returns `this`. A half-drained iterator can therefore
  be handed to `for-of` or to spread, which is what a real generator object
  does.

The whole of it reuses what docs/0021 already built. `@@iterator` is
docs/0021 decision 1's well-known string key, so `iter.open` takes the
`Protocol` kind and runs 7.4.2 GetIterator for real; the result object is an
ordinary object literal; the runtime rule that makes an own key beginning
with `@@` non-enumerable is what keeps the method out of `Object.keys` and
`for-in`. **There is no second protocol.** There is also no `yield` node in
the AST, no new IL op, no runtime change and no ABI change — inference,
lowering and the backend never learn that generators exist.

### What the subset admits

A generator body is a sequence of statements in which

- every `yield <expr>;` is an expression statement at the TOP LEVEL of the
  body, and
- no statement at that top level declares anything.

Statements between two yields are kept and run in the segment they were
written in, so `console.log('a'); yield 1; console.log('b'); yield 2;` logs
`a` on the first `next()` and `b` on the second — the laziness is real, not
approximated. Statements after the last yield run on the call that reports
`done`, exactly once, which is what the guard around the trailing segment is
for.

### Why a declaration at the top level is refused

This is the one restriction that is not in the measurement, and it is the one
worth the words. `next` is re-entered from the top on every call, so a
binding declared in one segment is gone by the time the next segment runs:

```js
*g() { const a = compute(); yield a; yield a + 1; }   // refused
```

Hoisting such a binding into the enclosing invocation would make it work, and
would also make the parser a scope analyser — it would have to decide what
`const [a, b] = ...`, a `var` and a nested `function` declaration each mean
when lifted, and get the answer right for all of them. The alternative to
refusing it is a binding that silently reads `undefined` on the second step,
which is docs/0000's exact failure mode. So it is a named error, and a
program that wants it can write the value into the yielded expressions.

### Every refusal is its OWN message

`yield` is contextual — not a reserved word — so outside a generator it stays
an ordinary identifier and `const yield = 1;` still parses. Inside one, the
parser knows which top-level statement it is in the middle of, and names that
construct:

| Written | Message names |
|---|---|
| `for/while/do` around a yield | ``a `yield` inside a loop`` |
| `if` around a yield | ``a `yield` inside an `if` `` |
| `switch` around a yield | ``a `yield` inside a `switch` `` |
| `try` around a yield | ``a `yield` inside a `try` `` |
| a bare block around a yield | ``a `yield` inside a nested block`` |
| `yield*` | ``` `yield*` (delegation) ``` |
| `const x = yield v;` | ``a `yield` inside an expression whose value is used`` |
| `return <expr>;` | ``` `return <expr>;` in a generator ``` |
| `return;` | ``` `return;` in a generator ``` |
| `let a = 1;` at the top level | `a declaration at the top level of a generator body` |
| `{ *g() {} }` | `a generator method in an object literal` |

Every one of them ends with what the subset IS, so the message teaches rather
than merely refuses. `const x = yield v;` is deliberately parsed BEFORE it is
refused, so that it reports the yield whose value it reads rather than the
declaration that reads it — two true statements about one line, and the
useful one is the yield.

`cases/blocked/generator_general.js` holds the hand-derived expectations for
the general thing, and says what a general implementation would take: every
binding that crosses a yield lifted into a state object, the body cut into
basic blocks at each yield, and `next` a dispatch over the block index with
the resumption value delivered as its parameter. That is an IL-level
transform, not a parser one.

### Which forms are covered

Generator FUNCTIONS cost almost nothing on top of the methods — the body
desugaring is identical and only where the value lands differs — so
`function* g() {}` and `const g = function* () {};` are both built.
`*name() {}` in a class body is built alongside `*[Symbol.iterator]()`. A
generator method in an OBJECT literal is a named error: its home object is
the literal, three.js's six generators are all class members, and an unpinned
construct is how docs/0000's plausible-but-wrong answers got in.

## Decision 2 — `[Symbol.iterator]` is matched syntactically, and is the only computed member name

A class body had no computed member names at all. Rather than build the
general form — which means evaluating an expression for a key at class
definition time, and a `MethodDef` whose key is a value rather than a
constant — the parser matches the four tokens `[ Symbol . iterator ]` and
produces the member name `@@iterator`.

That is the same bet docs/0011 decision 1 already makes for every provided
global: the name is resolved at compile time, and a program that rebinds
`Symbol` gets bronze's answer anyway. It is recorded here as a divergence
rather than hidden. Every other computed member name keeps its named error.

The match is used with and without the `*`, so
`[Symbol.iterator]() { return this.it; }` — an iterator written out by hand —
works too. One rule about what a computed class member name may be, not a
generator-only one.

## Decision 3 — the desugaring lives in the PARSER

The alternative was an AST→AST pass, or lowering. The parser wins for one
reason: it is the only stage that can refuse a construct at the position it
was written, with the enclosing statement still on the stack. A lowering-time
refusal would have to reconstruct "this yield is inside a loop" from a tree
that already lost which loop the reader was looking at, and the messages in
decision 1 are most of the value of this chunk.

It also means `ast::dump` shows the desugared tree, which is what a reader
bisects with: the step variable, the arrow, and the two properties are all
visible in `bronze parse` output rather than hidden behind a node that
prints as `(generator ...)` and lowers to something else.

The precedent is the implicit constructor (docs/0017 decision 7), which the
parser synthesizes for the same reason: lowering wants one shape of class,
always.

## Decision 4 — the receiver of `o[k]()`, which was missing

`v[Symbol.iterator]()` is how a program takes an iterator by hand, and it is
a call through a computed member expression. bronze passed **no receiver at
all** through that form: lowering had a branch for `o.m(...)` and fell
through to the plain-callee branch for `o[k](...)`, so `this` inside the
method was undefined and every `this.x` in it read `undefined`.

That is a silent wrong answer, not a hard error, and it is the worst kind
this project can ship. ECMA-262 13.3.6.1 evaluates the MemberExpression once
and passes its base as the this value, and 13.3.3 makes no distinction
between `o.m` and `o[k]` about that.

The fix is a branch in `lowerCall` mirroring the member one, reading through
a new `emitIndexRead` that `lowerIndexAccess` also uses — shared precisely so
the base is lowered ONCE and the two paths cannot disagree about which key a
literal index folds to. `cases/computed_call_receiver.js` pins the receiver,
the single evaluation of the base, the base-then-key-then-arguments order,
and the `o?.[k]()` short circuit.

It is written down here rather than left in the commit message because it was
found by this chunk and is not about generators: any `o[k]()` in any program
was affected.

## Decision 5 — `warn` and `error` write to stderr, and that is the load-bearing part

`console.warn` and `console.error` were `undefined variable: console`. They
are now the same formatter as `console.log` — the same inspect format
(docs/0013), the same single-space join, the same terminator — pointed at a
different stream.

The stream is not a detail. The oracle harness pins stdout **byte-for-byte**
across every case in the suite (docs/0003). If a library's warnings landed on
stdout, then every future three.js-derived case would bake that chatter into its
expectation, and the ratchet would start protecting noise instead of
behaviour. `cases/console_streams.js` therefore pins a NEGATIVE: it writes
distinctive strings through `warn` and `error`, and none of them appears in
the pinned file. A later refactor that merges the streams fails that case,
which is the only way that mistake gets caught — a merged stream is
otherwise indistinguishable from working code.

The destination is a `FILE*` parameter threaded through every writer rather
than a module-level variable the entry points swap, for the same reason: a
stream held in a variable is state, and state can be left pointing at the
wrong file by a future early return.

`Op::PrintErr` is its own IL op rather than a flag on `Print`, because the
canonical dump is what a reader bisects with and a stream carried in a field
is a stream the dump can silently omit.

## Decision 6 — `info` and `debug` are `log`; `trace` and everything else are named errors

`console` is not in ECMA-262; the WHATWG Console Standard §2.1 defines `log`,
`info` and `debug` as one operation with a different severity hint, and a
severity hint is something a terminal filter reads, not something a compiler
emits. So all three are stdout and share `Print`.

`console.trace` is a named error: it prints a stack trace, and bronze does not
build one. Emitting the message without the trace would be a plausible wrong
answer.

Every other member — `table`, `group`, `time`, `assert`, anything — is a hard
error naming itself, which is docs/0011 decision 3's rule applied to
`console`. It replaces `undefined variable: console`, which named the wrong
thing. `ast::consoleStreamOf` is the single table that decides both which
members exist and where each one's output goes: the parser folds
`console.<m>` into one `Ident` and lowering dispatches on that name, so the
two cannot drift into a `console.warn` compiled to stdout.

The divergence, unchanged from before: `console` is folded at the member
expression and is not a binding, so `const console = {...}` does not shadow
it and `const f = console.log` is still an error.

## Decision 7 — an arrow does not get a receiver slot of its own

Found by decision 1, and worth its own entry because it is not about
generators: `this` inside an arrow inside an arrow read `undefined`.

An arrow reads the enclosing receiver out of the environment chain under the
name `"this"` (docs/0012 decision 3), and `enterFunctionEnv` gave a slot of
that name to any function body with an arrow that mentions `this` in it.
Including an ARROW's own record — which has no receiver to put in the slot,
because an arrow takes no `__this` parameter, so the entry copy wrote
`undefined` into it. An arrow one level deeper then found that empty slot
first and stopped walking.

ECMA-262 10.2.1.1 says an arrow has no `[[ThisMode]]`: it never binds a
receiver, so it must never own the slot. One line, and the walk reaches the
function that does bind one at whatever depth it sits.

It matters to decision 1 beyond its own sake: the desugaring puts every
yielded expression inside an arrow, so `yield this.items.map(v => v * this.k)`
— which is depth one when written by hand — becomes depth two, and would have
compiled to a `TypeError` reading a property of `undefined`.
`cases/nested_arrow_this.js` pins one, two and three arrows deep, an arrow
inside an ordinary function expression stopping at that function, and two
receivers not leaking into each other.

## Oracle cases

- `generator_iterator.js` — the three.js shape: `for-of`, spread and
  destructuring over one class; a second walk starting fresh; nested loops
  over one iterable; per-step evaluation; two receivers on one prototype
  method; the method invisible to `Object.keys` and `for-in`.
- `generator_next_protocol.js` — `next()` by hand, the result object's shape
  and key order, the two calls past the end, the iterator as its own
  iterable, and two iterators from one instance advancing independently.
- `generator_function.js` — `function*` and the function-expression form,
  when each stretch of the body runs, the trailing statements running once,
  interleaved walks, `yield;` with no operand, and the empty generator.
- `console_streams.js` — the stdout three, and the negative assertion for
  the stderr two.
- `computed_call_receiver.js` — decision 4.
- `nested_arrow_this.js` — decision 7.
- `blocked/generator_general.js` — the general generator, and what it needs.

The refusals are pinned in `tests/parse/parser_generator_test.cpp` rather than
as oracle cases, because a case that does not compile has no stdout to pin.

## Files this chunk added, and the seams it cut

- `src/parse/parser_generator.cpp` — the generator production and its
  desugaring, including the `[Symbol.iterator]` key match. It is the only
  file that knows what a `yield` is.
- `tests/parse/parser_generator_test.cpp` — the refusals.
- `tests/parse/parser_{expr,asi,pattern,literal,stmt}_test.cpp` — the parser
  test file crossed 950 lines while this chunk was in it, and was split along
  the seams `src/parse` already names, one test file per grammar region. A
  pure move: same assertions, same text, same 553 of them.
