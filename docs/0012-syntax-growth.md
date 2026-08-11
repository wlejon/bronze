# 0012 — Syntax growth: template literals, for-of, arrows, classes

Status: template literals, for-of and arrow functions landed 2026-08-11;
classes designed here and not yet built. This is the second half of phase 4
of docs/0001. docs/0011 covers the other half (the globals and the prototype
methods); the split is deliberate — that doc is about what a program can
*call*, this one is about what it can *write*.

The bar is three.js. Its source is ES2015+ throughout: every class is a
`class`, every callback is an arrow, and `for (const v of list)` is how it
walks anything. None of that parsed before this doc.

## Decision 1 — a string literal is decoded once, by one decoder

Before this step bronze never decoded escapes at all: `"a\tb"` reached the
runtime as the four characters `a`, `\`, `t`, `b`, and printed that way.
That is a silent wrong answer, which docs/0000 lists as the failure mode
worth the most vigilance, and it survived because no case printed an escape.

There is now one `Parser::decodeStringLiteral(raw, span)`, and every site
that turns literal source text into a string value goes through it: quoted
literals, string keys in object literals, and each cooked piece of a
template. `\x41`, `A`, `\u{1F600}`, the single-character escapes and
line continuations are handled there and nowhere else; an unknown escape is
its own character, per ECMA-262. A second decoder is how the three spellings
would drift apart, so there is not one.

A template is lexed as *pieces* rather than as one token: `TemplateWhole`,
`TemplateHead`, `TemplateMiddle`, `TemplateTail`. The lexer keeps a stack of
brace depths for open substitutions, so a `}` closes a substitution only
when the innermost open template is at depth zero — which is what lets an
object literal appear inside `${ }`. The parser stores quasis and
substitutions in an AST node whose quasi count is always one more than its
substitution count, and lowering emits a left-to-right chain of dynamic
`+`: each substitution is ToString'd exactly where the language says it is,
and no fast path may reorder that.

## Decision 2 — for-of is an index walk, not an iterator protocol

There are no symbols, no `Symbol.iterator`, and no generator objects, so
there is no protocol to dispatch through. `for (const x of it)` lowers to a
counted walk over three ops:

- `iter.length it` — re-read at the top of EVERY iteration, so a body that
  pushes or pops sees its effect, as the language requires.
- `iter.at it, i` — the element (an array element, or the code point at a
  string offset).
- `iter.advance it, i` — the next cursor.

`advance` exists as its own op rather than as `i + 1` because a string
iterates by code *point*: a surrogate pair steps the cursor by two units and
yields one character. Folding it into an increment would produce two lone
surrogates for every astral character — again, a silent wrong answer.

The loop is four blocks (header / body / update / exit) on the same
block-argument SSA as every other loop (docs/0005), with the cursor carried
as an extra block argument that `continue` must pass along. The binding is
per-iteration: the closures created in `for (const x of a) fns.push(() => x)`
each capture their own `x`, which falls out of entering a fresh scope per
iteration exactly as the `for (let ...)` loop already did (docs/0007).

Arrays, strings and typed arrays are what bronze can build, and they are
what the walk covers. Anything else is the hard error `for-of over a value
that is not an array, string or typed array` — never an empty loop, which is
what a missing-protocol fallback would silently produce.

## Decision 3 — an arrow captures `this`; it does not receive one

An arrow function is not a shorter function expression. It has no `this` of
its own, and `this` inside it means whatever it meant in the enclosing code
— including at the top level, where it means `undefined` rather than a fresh
binding.

The mechanism is the one closures already use (docs/0007), with no new
machinery: every function's environment record reserves slot 0 for `this`,
and the prologue copies its own `__this` parameter into it. An arrow is then
lowered like any other closure except that it takes NO `__this` parameter,
and a `this` in its body resolves to an `env.get` of that reserved slot,
walking out as many environment levels as needed. Capture analysis treats
`this` as a captured *name* — a name no source can spell, because `this` is
a keyword, so it can never collide with a binding.

Two consequences fall out for free and are pinned: an arrow's `this` cannot
be rebound by its call site (there is no parameter to rebind), and an arrow
stored on an instance in a constructor still sees the instance when it is
called bare later.

The AST dump prints an arrow under its own head (`arrow-expr`) rather than
as `function-expr`. Two constructs that lower differently must not dump
identically.

Detection is by lookahead at the *operand* entry point, not in the
expression parser: `(a, b) =>` and `x =>` both have to be recognized before
their leading tokens are consumed as a parenthesized expression or an
identifier, and an arrow can appear anywhere an operand can — including as
the right side of an assignment, which is a binary operator here and so
never passes back through the top of the expression grammar
(`this.get = () => this.count`).

## Decision 4 — a `new` expression is a receiver

`new Point(1, 2).scale(3)` is one member call on a fresh object. The `new`
parser used to return straight to its caller, so the `.` after the
constructor's argument list read as a syntax error. The suffix loop (`.p`,
`[i]`, `(args)`, `++`, `--`) is now shared, and `new` runs its result
through it like any other primary. The callee itself is still an identifier
only, which stays a named error.

This is a prerequisite for classes rather than a convenience: the pinned
`class_basics` case calls `new Point(1, 2).scale(3).sum()`, and three.js
writes that shape constantly.

## Decision 5 — classes are sugar over what docs/0008 already built

**Not built yet. This is the plan the `class_basics` blocked case holds the
compiler to.**

docs/0008 already has constructor functions, `prototype` objects, the proto
chain, `new`, and `this`. A `class` declaration therefore introduces no new
runtime concept — it is desugared in lowering:

- `class C { constructor(a) { ... } }` becomes the constructor function `C`.
  A class with no constructor gets an empty one (or, under `extends`, one
  that forwards its arguments to `super`).
- each method becomes a function stored on `C.prototype`, non-enumerable in
  the language and — until docs/0009 grows property attributes — stored the
  same way an ordinary assignment stores it, which the enumeration-order
  case must be extended to cover before classes promote.
- `static` members are stored on `C` itself.
- `extends B` links `C.prototype`'s proto to `B.prototype` and `C`'s proto
  to `B`, so static members inherit too.
- `super(...)` is a call of the parent constructor with the current `this`;
  `super.m()` is a lookup starting at the parent prototype but called with
  the current `this`. Both need the *home object* of the enclosing method,
  which is the one genuinely new piece of state — it is a compile-time
  constant per method, not a runtime lookup.
- a field initializer (`x = 1` in the class body) runs at the top of the
  constructor, in source order, after `super()`.

What is deliberately out: private `#names`, getters and setters (they need
property attributes first), computed method keys, and decorators. Each stays
a named error.

## Named diagnostics

- `for-of over a value that is not an array, string or typed array` —
  decision 2, at runtime, for an iterable bronze cannot walk.
- `unsupported construct: class declaration` / `class expression` /
  `super (classes are not built yet)` — decision 5, until it lands. `class`,
  `extends` and `super` are lexed as keywords for exactly this: before them
  a class body reported a missing semicolon and named nothing. `static` is
  deliberately NOT a keyword — it is not reserved in JavaScript, and taking
  it would break `obj.static`.
- `unsupported construct: tagged template literal` — the tag form is not
  decision 1's cooked-pieces path and is not built.
- `unsupported construct: for-in loop`, `switch statement`,
  `try/catch/throw`, `delete` — unchanged, and still the phase-4 backlog.
