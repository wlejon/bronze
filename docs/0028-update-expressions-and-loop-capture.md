# 0028 — Updating a property, and which closure really captures a loop binding

Status: implemented. Part of phase 4 of docs/0001, and the chunk that turns
"the three.js app resolves" into "the three.js app links and runs".

Two constructs stood between bronze and a compiled build of unmodified
three.js 0.160.0, and neither was a missing subsystem.

```js
// three.js materials/Material.js:95
this.version ++;

// three.js core/BufferGeometry.js:816
for ( let i = 0, il = morphAttribute.length; i < il; i ++ ) { ... }
```

The first was diagnosed by name and unbuilt. The second was diagnosed by name
and *wrongly* — it compiles fine, and the check that rejected it was asking a
question that only shared a spelling with the real one.

## Decision 1 — an update expression is a reference, not a read plus a write

`o.k++` is not `o.k = o.k + 1`. ECMA-262 13.4.2.1 evaluates the target to a
**Reference** once, calls GetValue on it, calls ToNumeric on the result, adds
or subtracts one, and PutValues through the *same* reference. The rewrite
spells the reference twice, and every place a reference has a side effect is a
place the two differ:

- `f().k++` calls `f` once; `f().k = f().k + 1` calls it twice.
- `a[i++]++` advances `i` once; the rewrite advances it twice and reads one
  element while writing another.
- `o[key()]++` calls `key` once, and the key it returned is used for both
  halves.

So the update path lowers the base — and, for an index, the index — to a value
and then names that value in both the get and the set. That is exactly what
`lowerAssignment` already does for `o.k += 1`, and the two now share their
shape deliberately: a literal index takes the inline-cached `PropGet`/`PropSet`
pair in both, so `a[0]++` and `a[0] += 1` describe one site and not two.

`this.k++` in a method falls out of the same rule with nothing extra, and so
does an accessor pair: one base value means the getter and the setter run on
the same receiver, which is docs/0019 decision 4 restated from the other side.

The lowering lives in `src/lower/lower_update.cpp` rather than beside the rest
of the unary operators, because the reference is what makes it hard and no
other unary operator has one.

## Decision 2 — ToNumeric first, and generated code gets ONE ToNumber

13.4.2.1 step 2 is `ToNumeric(GetValue(ref))`, before the arithmetic and
before the write. That is observable: `o.s = "5"; o.s++` leaves the **number**
6 where `o.s = o.s + 1` would have concatenated to `"51"`, and the expression's
value is the number 5 rather than the string.

bronze had two ToNumbers. `rtToNumber` in the runtime implemented the spec and
the builtins called it; `bronze_unbox_f64`, the only numeric coercion
generated code can reach, was a second, smaller copy that agreed with it on
four tags and hard-errored on a string — so `+"5"` and `o.k = "5", o.k--` were
fatal errors sitting next to a `Number("5")` that answered 5, and an
int32-tagged value fell through to the fatal too. The second copy is gone;
`bronze_unbox_f64` is `rtToNumber`. ToPrimitive on an object is still unbuilt
and is still a named hard error, which is the only case that should be one.

This is the docs/0001 "hard errors over silent fallbacks" rule pointing the
other way for once: the error was not protecting against an unimplemented
construct, it was standing in for one that the codebase had already
implemented ten feet away.

## Decision 3 — the per-iteration capture test, stated precisely

ECMA-262 14.7.4.9 gives a `for` whose head is a **LexicalDeclaration** a fresh
copy of each binding before every iteration, with the previous value copied
in. A closure created in the body captures that iteration's copy. bronze
carries a loop variable as a block parameter across the back edge (docs/0005)
— one binding by construction — and creates an environment record on scope
*entry* (docs/0007 decision 2), which for the loop header happens once above
the header block. So a genuine capture of a `let` loop binding would silently
share one cell, and it is refused by name.

The refusal was right; the test for when to apply it was not. It fired when

> any closure anywhere in the enclosing function mentioned the loop binding's
> **name**,

which rejects `for (let i = 0, il = a.length; i < il; i++)` sitting next to
`a.map((x, i) => …)` — two `i`s that share nothing but four pixels. That shape
is everywhere in real JavaScript, and rejecting it rejected a large fraction of
ordinary code.

The new rule:

> A `for` head's **lexical** binding is refused when a function nested inside
> that `for` statement — its head, condition, update or body — references the
> name **freely**: mentions it without binding it itself.

Three parts, each of which was doing work:

- **`for` statement, not enclosing function.** A closure outside the loop
  cannot see a binding the loop declares. It can see an *enclosing* binding of
  the same name that the loop shadows, which is a different fact and is
  handled by decision 4.
- **freely, not merely mentioned.** `functionFreelyReferences` in
  `src/ast/queries.cpp` subtracts the closure's own bindings and recurses at
  every nested function, so a parameter named `i` two closures down shadows for
  everything under it.
- **lexical, not `var`.** 14.7.4.9 copies only a LexicalDeclaration. `for (var
  i = …)` declares **one** binding for the whole loop, which is precisely what
  bronze already produces, so a closure over it was always correct and is no
  longer diagnosed. Every such closure sees the value the loop left behind.

### What "binds it itself" is allowed to mean

`functionBindsName` counts parameters, the body's own top-level lexical and
function declarations, and a top-level `var`. It deliberately does **not**
count a declaration written inside a nested block, in both directions:

- a `let` in an inner block covers only that block, so `{ let x; } use(x)`
  still reaches outward — counting it would hide a real capture;
- a `var` in an inner block *is* function-scoped by 8.6.2 and so does cover
  the whole body — but bronze does not hoist one out of the block it is
  written in (see the known bug below), and this predicate has to describe the
  bindings lowering actually makes, not the ones the spec describes. The first
  version counted them, and a closure whose `var i` sat one block down was
  compiled against a shadow that did not exist: it captured the loop's `i` and
  printed `0,1` where the answer is `7,7`.

That is the asymmetry to keep in mind whenever this predicate is touched: a
false "free" costs a diagnostic on a program that would have worked; a false
"bound" is a silently wrong capture. When it cannot tell, it says free.

Pinned on both sides — `for_loop_binding_shadowing` for the shapes that must
now compile, `blocked/for_loop_per_iteration_binding` for the escaping capture
that must still be refused.

## Decision 4 — the `for` head is a scope, so it needs a record of its own

Relaxing decision 3 exposed a latent aliasing bug that the old over-rejection
had been hiding.

```js
function f() {
  let v = 99;
  const read = () => v;      // captures the OUTER v, so `v` is env-backed
  for (let v = 0; v < 3; v++) { }
  return read();             // must be 99
}
```

`lowerForStmt` entered the header's scope with the plain `enterScope()`, which
creates no environment record. `declareVariable` gives a captured declaration
the slot the innermost record holds for that name — and the innermost record
was the *function's*, which already had a slot for the outer `v`. The header's
`let v` was therefore handed the storage of the binding it shadows, and
`read()` would have returned 3.

The header now enters with `enterScope(forStmt->init, ilFn)`, so
`getScopeDeclarations` sees the head's own declarations and gives them slots of
their own when they are env-backed. A block statement has always done this;
the `for` head is a scope for exactly the same reason and was the one that
did not.

## Decision 5 — `return;` returns a value

14.10.1: a `ReturnStatement` with no expression returns **undefined**, which is
a value. bronze emitted a void return for it whatever the function's IL return
type was, so a function that mixed the two forms — the guard clause —

```js
function early(x) {
  if (x === undefined) return;
  return x * 2;
}
```

put a `ret void` in a body typed to return an i64. The LLVM verifier rejected
it and the whole module was refused; nothing before the backend noticed.
Falling off the *end* of the same function already produced the undefined
(`lowerFunctionBody` has done so since docs/0005), so this was one door into a
rule the other door already obeyed.

It is recorded here rather than left for a later chunk because it is what the
first two decisions uncovered: with `++` and the loop check fixed, this was the
only thing between the three.js app and a linked executable, and it appears in
Object3D, EventDispatcher, Material, BufferGeometry, Mesh and Quaternion.

## Decision 6 — the provided Error classes get the back-pointer too

docs/0008 installs `prototype.constructor` on every function lowering builds,
which is 10.2.5 step 6. The Error family is built by the runtime instead
(docs/0020 decision 7) and was built without it, so `e.constructor` was
`undefined` on every error — the one bronze raised and the one the program
constructed alike. `e.constructor === TypeError` is how idiomatic code asks
what it caught, and the answer was silently wrong rather than loudly missing,
which is the worse of the two failures.

It is a **definition** and not an assignment, because `TypeError.prototype`
inherits from `Error.prototype`: a plain set would have found the parent's
property and written through it, giving every class the last one built.

`e.constructor.name` still throws — `Function.prototype.name` is a named hard
error and is a separate piece of work — but the identity comparison, which is
the discriminating idiom, now answers.

## Where this leaves the three.js target

The app — unmodified three.js 0.160.0, a `Scene`, a `PerspectiveCamera`, a
`BoxGeometry`/`MeshBasicMaterial` `Mesh`, sixty frames of
`updateMatrixWorld()` — **compiles and links**, with warnings only for the
typed-array globals. It runs to `Uncaught ReferenceError: Uint32Array is not
defined`, which is the next chunk and not a bug in this one.

## Known bugs this chunk found and did not fix

- **A `var` inside a nested block is not hoisted to the function's scope.**
  `function g() { if (true) { var j = 6; } return j; }` warns "unresolved name
  'j'" and throws a ReferenceError at run time; the same `var` at the
  function's top level works. Loud rather than silent, and the reason
  `functionBindsName` above is written the way it is.
- **A sparse array write is a named hard error.** `arr[5]++` on a
  three-element array reports "sparse array write (index past the end) is
  unsupported until dictionary elements land" rather than growing the array.
- **An elision in an array literal is a named hard error**, so `[0, , 2]` does
  not compile and array holes can only be made by other means.
- **`Function.prototype.name` is a named hard error**, so `e.constructor.name`
  still fatals even now that `e.constructor` answers.
- **`Error.prototype.toString` is missing**, so `e.toString()` is "undefined is
  not a function".
