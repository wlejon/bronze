# 0025 — `new` on any callee

Status: implemented.

`ast::NewExpr::callee` was a `std::string`, so `new` accepted a bare
identifier and nothing else. Sweeping the compiler over three.js left 47
files failing to parse and **43 of the 56 errors were this one construct**,
split across two messages that were one cause — "new with a non-identifier
callee" (32) and "new requires an argument list" (11, which is `new a.b.c(`
reaching the paren check with the member chain still unconsumed). Every
"expected '}' to close block" in that sweep was knock-on.

The construct is not exotic. It is how prototype-style code clones an object
without naming its class:

```js
return new this.constructor().copy( this );                 // math/Box3.js
this.array = new source.array.constructor( source.array );  // core/BufferAttribute.js
new Curves[ 'QuadraticBezierCurve3' ]( a, b, c )            // geometries/TubeGeometry.js
```

## Decision 1 — the callee is an expression, and the grammar is the whole design

`NewExpr::callee` becomes an `ExprPtr`. ECMA-262 13.3 spells the production

```
MemberExpression : new MemberExpression Arguments
NewExpression    : MemberExpression | new NewExpression
```

which is one rule with four grouping consequences, and every one of them is a
*different tree for the same tokens*. They fall out of a single omission: the
callee is parsed by the ordinary suffix chain **minus the call**, because the
first `(` at that level is the `new`'s own ArgumentList.

| written | constructs | then |
| --- | --- | --- |
| `new a.b.c()` | `a.b.c` | — |
| `new a.b().c` | `a.b` | reads `.c` off the instance |
| `new (f())()` | the call's result | — |
| `new a[i]()` | the computed member | — |
| `new new F()()` | `new F()`'s result | — |

`parseNewCallee` is that member-only loop; `parseNewCore` is `new
MemberExpression Arguments?` without the trailing suffix chain; `parseNew` is
`parseNewCore` followed by it. The split is what makes `new new F()()` work —
a nested `new` that ran its own suffix chain would take the outer `new`'s
argument list as a *call* on the inner instance, which is a plausible-looking
tree and a wrong answer.

The `.name` / `[expr]` links are built by one shared `parseMemberLink` rather
than a second copy inside the callee loop, because two copies would
eventually disagree about what `new a.b[c]()` constructs.

## Decision 2 — `new Foo` without an argument list is supported, not diagnosed

It is the `new NewExpression` production, and 13.3.5.1 evaluates it with an
empty argument list. Diagnosing it would be a false error on valid code,
which is worse than the alternative here: the two forms are *semantically
identical*, so supporting it costs one `if` and creates no second behaviour to
reason about. `new Foo` and `new Foo()` therefore produce the same tree,
deliberately.

The superseded assertion ("new without an argument list is a hard error") is
replaced by one pinning that the two spellings dump identically, not deleted.

## Decision 3 — "is this a constructor" moves to the value, but the name still buys the shape

Lowering already routed `new` through one `Op::Construct` over a callee value,
with the whole ceremony behind the runtime helper (docs/0008 decision 4). That
is exactly what makes the widening free: an arbitrary callee expression lowers
to whatever produces its value and then reaches the *same* instruction. The
"callee must be a function object" check was already a run-time check on a
value; nothing moved.

What a name still buys is in `src/types`. `FlowAnalyzer::newExpr` interns a
shape class from the constructor's `this.x = ...` assignments and records it
against the site, which is what makes property accesses on the result
monomorphic (docs/0010). A shape class *is* a claim about constructor
identity, and only a bare `Ident` callee lets the analysis name the function.
So:

- an `Ident` callee: unchanged — `constructorShape(name)`, same site shape,
  same inline caches. `new Foo()` does not move onto a slower path.
- any other callee: `kNoShapeClass`, which is the answer an unknown name
  already produced. The site's property accesses stay polymorphic and guarded,
  which is sound; guessing a layout would not be.

The non-`Ident` callee is also *walked* for its flow effects, before the
arguments, which is the order 13.3.5.1 evaluates them in. A bare name is
deliberately not walked: reading it is not what `new` does with it, and the
`constructorShape` call is this site's contribution about that name.

`Float32Array` and `ArrayBuffer` keep their by-name special case in lowering
and are now guarded on the callee being an `Ident`, because there is no
function object behind either one (docs/0011) — so only a bare identifier can
ever be one, and `new lib.Float32Array()` is an ordinary construction of
whatever that property holds.

## Decision 4 — an `Ident` callee prints on the head line

`ast::dump` prints `(new Foo` for an identifier callee and `(new` with the
callee as its first child otherwise. Two reasons, one of which is a ratchet:

- it is the form decision 3 treats specially, and seeing that at a glance is
  what the dump is for — the same reason a member access prints its property
  inline;
- every pinned dump in the tree stays byte-identical, which is the evidence
  that this was a *widening* and not a change of behaviour. An expectation
  that moved would have meant `new Foo()` no longer parses the way it did.

## Decision 5 — the renamer recurses, and it is the highest-stakes edit here

`modules/rename.cpp` called `rewrite(nw->callee)` on the string. It now calls
`expr(*nw->callee)`. Getting this wrong is not a compile error: an unrenamed
base in `new imported.Ctor()` binds to whatever the *importing* file happens
to call `imported`, which is a silent wrong binding across module files
(docs/0023 decision 1 flattens the graph into one scope, so the name exists).

It is pinned twice, on purpose. `tests/modules` pins the merged dump — the
base renamed, the property name beside it left alone because a key is never a
binding, and both halves of a computed callee renamed. `tests/oracle/cases/
module_new_callee` pins the **values**, with two modules exporting the same
names under different constructors and a function-local binding shadowing the
import, so a walk that renamed too much and a walk that renamed too little
each print something different.

## Decision 6 — `Foo.prototype.constructor` had to exist

`new this.constructor()` — the case the chunk exists for — was
`undefined is not a constructor`, because bronze never installed the
back-pointer ECMA-262 10.2.5 (MakeConstructor) step 6 puts on every
constructor's prototype. That is a hard error, but a *wrong* one: the language
says the property is there.

It is installed at the two places a prototype object is minted —
`rtEnsureFunctionPrototype` and `bronze_class_extends`, the latter because a
derived class replaces the prototype object and would otherwise inherit the
base's back-pointer and clone the wrong class. Non-enumerable, with the
attributes 10.2.5 names, so `Object.keys` does not report it and a `for-in`
over an instance does not visit it. Defined rather than assigned, so a base
prototype's `constructor` is never written through.

The whole oracle suite — including under GC stress — produced identical bytes
afterwards, which is the evidence that a reference cycle
(prototype → function → prototype) and one more property on every prototype
changed nothing observable that was already pinned.

Deliberately **not** pinned: what `constructor` reads as after
`Foo.prototype = {...}`. The spec answer routes through `Object.prototype`,
which bronze does not have (docs/0008), so the two agree on `false` for
different reasons and an expectation there would be pinning a coincidence.

## Named hard errors, not silent fallbacks

- `an optional chain may not be the callee of 'new'` — 13.3 has no
  OptionalExpression under `new`, because the short circuit would have to hand
  `new` a value to construct and there is none.
- `unsupported construct: new.target` — a MetaProperty, not a construction at
  all; it asks how the enclosing function was invoked.
- `unsupported construct: async method in a class body` — see below.

## Two more from the same sweep

**A UTF-8 BOM was `unrecognized character`.** U+FEFF is `<ZWNBSP>`, which
ECMA-262 12.2 lists in *WhiteSpace* — so it is trivia **wherever** it appears
and not only as a leading signature, and it is handled as one rule in
`skipTrivia` rather than a special case at offset zero. It is not a
LineTerminator (12.3 lists those separately), so it leaves ASI alone.

**`async` methods were diagnosed as class fields.** `async loadAsync(url) {`
is identifier-then-identifier, the same token shape `x = 1;` has, so the field
diagnostic claimed it. three.js contains **zero** class fields and eleven
async methods, so that message was wrong every time it fired. `async` stays
contextual — 15.8.1 makes it a modifier only when a ClassElementName follows
on the same line, so `async() {}` is still a method named `async` and
`async = 1` still a field. `async`/`await` themselves are **not** implemented
and are not wanted here: 5 uses in 2 files, both loaders/WebXR, off the
critical path. The point was an accurate name, not a feature.

## Refused

**A destructuring assignment whose target is a member expression** —
`( { a: this._x, b: this._y } = f() )`, one site in all of three.js
(`extras/PMREMGenerator.js`). It stays the named hard error it was.

It is not cheap in the way the other two were. Every target a
`BindingPattern` can hold today introduces or rebinds a *name*; a property
target needs the receiver evaluated, and 13.15.5 evaluates each element's
target **reference** before reading its value, interleaved left to right with
the rest of the pattern. That is a new field on `PatternElement` threaded
through pattern lowering, the renamer, the scope queries and the assignment
walker, for one call site — and pattern lowering is precisely where a
mis-ordered evaluation is a silent wrong answer rather than a crash. It wants
its own chunk beside the rest of 13.15.5, not the tail of this one.

## What shipped

`tests/parse/parser_new_test.cpp` — the grammar, split from `parser_test.cpp`
along the seam this production names. Every grouping is written so a wrong
reading yields a visibly different tree.

Oracle cases, each run with inference and with `--no-infer`, and each also
under `oracle-gc-stress`:

- `new_callee_grouping` — every grouping above exercised for its **value**,
  with each candidate callee a real constructor tagging which one ran, so a
  consistently-wrong parser prints a different line rather than passing a tree
  assertion. Plus the empty argument list, callee-before-arguments evaluation
  order, and a non-constructor callee as a run-time `TypeError`.
- `new_constructor_property` — `new this.constructor()` cloning through a base
  and a derived class, the back-pointer's identity and non-enumerability, an
  ordinary function's, reaching a class through an instance, and a reassigned
  `.prototype`.
- `module_new_callee` — decision 5, by value.

Measured after: on a three.js source tree (685 files), 25 files still fail to
parse and **none of them for a `new`**. The remaining messages are generator
methods (7), async methods (11, now named correctly), `async function`
declarations, one destructuring target, and their knock-on.
