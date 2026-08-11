# 0012 — Syntax growth: template literals, for-of, arrows, classes

Status: template literals, for-of, arrow functions and classes all landed
2026-08-11. This is the second half of phase 4
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

docs/0008 already had constructor functions, `prototype` objects, the proto
chain, `new`, and `this`. A class therefore introduces no new runtime
concept: it is desugared in lowering, and everything below is something a
program could have written by hand.

- `class C { constructor(a) { ... } }` becomes the constructor function `C`,
  and the declaration binds that function to the name. A class that writes
  no constructor gets an empty one, synthesized in the PARSER, so lowering
  always sees exactly one.
- each method becomes a closure stored on `C.prototype`, which is read once
  per class and written to with the ordinary property path — so a method is
  one function object shared by every instance, and an inline cache treats
  `p.sum()` like any other property read.
- a `static` member is stored on the constructor function itself
  (decision 6).
- `extends B` is one instruction, `class.extend`, because both links have to
  be made together and BEFORE any method is stored: it replaces
  `C.prototype` with a fresh object whose proto is `B.prototype` (the
  prototype lives on the shape, docs/0004, so it is replaced rather than
  mutated), and links C's static properties to B's.
- `super(...)` is a dynamic call of the parent constructor with the CURRENT
  receiver — not a `new`. There is one object; the parent is only being
  given its chance to initialize it.
- `super.m()` reads `m` from the parent's prototype and calls it with the
  current receiver. Both halves matter: `this.m()` inside an override would
  find the override again and recurse forever, and calling the parent's
  function without the current receiver would initialize nothing.

The parent's name is resolved by the PARSER, from the class the method is
written in, and stored on the `super` node. That is what makes `super` a
compile-time constant rather than a runtime home-object lookup — and also
what makes the parent visible to capture analysis, which would otherwise see
a method body that mentions no such variable.

A method that never writes `this` still needs a receiver if it uses `super`,
because both super forms run on the current one. Missing that made
`super.describe()` report `this` outside a function.

What is deliberately out, each a named error: class fields (`x = 1` in the
body), getters and setters (they need property attributes first), computed
method keys, generator methods, private `#names`, class expressions, and a
DERIVED class with no constructor — that one desugars to
`constructor(...args) { super(...args); }`, and bronze has neither rest nor
spread, so forwarding fewer arguments than were passed would be a wrong
answer given quietly.

## Decision 6 — a function object carries its own properties

`class C { static m() {} }` stores `m` on the constructor function, and
before this a property write on a function object other than `prototype` was
a hard error ("unsupported until functions carry shapes"). Rather than give
every function a shape and slots, a function gets one more field: an
ordinary object holding its own properties, created on first demand exactly
as its `.prototype` is. A function that never receives a static member pays
nothing.

Static INHERITANCE then falls out with no extra mechanism: `class.extend`
gives the derived function's property object a prototype link to the base
function's, and the ordinary chain walk finds `B.make()` through `D`.

The read path is `prototype` first, then the property object, then the
unimplemented-member table — so `Function.prototype.call`, `bind` and
`length` stay named errors rather than becoming `undefined` now that a
function can answer property reads.

## Named diagnostics

- `for-of over a value that is not an array, string or typed array` —
  decision 2, at runtime, for an iterable bronze cannot walk.
- `unsupported construct: class field` / `class getter or setter` /
  `computed method name in a class body` / `generator method in a class
  body` / `class expression` / `a derived class with no constructor (write
  one that calls super)` — decision 5's exclusions, each named at the member
  that spells it. `class`, `extends` and `super` are lexed as keywords for
  exactly this: before them a class body reported a missing semicolon and
  named nothing. `static` is deliberately NOT a keyword — it is not reserved
  in JavaScript, and taking it would break `obj.static`.
- `unsupported construct: super outside a class method` and `super in a
  class with no 'extends'` — the two places the parser can see that a
  `super` has nothing to resolve against.
- `a class can only extend another class or a constructor function` —
  decision 5, at runtime, for `class C extends 5`.
- `unsupported construct: tagged template literal` — the tag form is not
  decision 1's cooked-pieces path and is not built.
- `unsupported construct: default parameter value` / `rest parameter` /
  `destructuring parameter` / `destructuring declaration` /
  `destructuring assignment` / `spread` — the ES2015 syntax the re-seeded
  blocked cases describe. `...` is lexed as ONE token so the parser can name
  it at all: as three dots it read as a property access of nothing. The
  parameter forms are named in one shared `parseParams`, which replaced four
  copies of the same loop — four places for these to drift apart. A
  destructuring ASSIGNMENT is the one form the parser cannot see, because
  both sides are ordinary expressions until the `=`, so lowering names it.
- `unsupported construct: for-in loop`, `switch statement`,
  `try/catch/throw`, `delete` — unchanged, and still the phase-4 backlog.
