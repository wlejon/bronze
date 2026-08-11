# 0018 — Selection, jumps, and the chain that stops

Status: implemented. Part of phase 4 of docs/0001.

Four constructs, one question. `for-in`, `switch`, labelled `break`/`continue`
and the optional chain all ask **which edge does control take out of here**,
and all four answer it with an edge that the structured lowering of docs/0005
does not otherwise produce: a jump out of the middle of a construct, to a
block chosen by something other than the nesting.

They landed together because they share machinery, not because they share a
theme. `for-in` reuses `for-of`'s index walk once the keys are a list.
`switch` is the first breakable statement that is not a loop, which is exactly
what forces `break`'s target search to become a stack search — and once it is
a stack search, a label is one extra field on the entry. The optional chain is
the odd one out syntactically and the same shape underneath: a set of edges
that skip the rest of an expression and meet at a join.

Before this chunk, `switch` and `for-in` were AST nodes whose bodies the
parser silently skipped past and lowering rejected by name; `LabeledStmt` did
not exist at all, and `break outer` was `unsupported construct: labeled
break/continue`; and `?.` lexed as `?` followed by `.`, so `a?.b` was reported
as a malformed conditional. Two blocked oracle cases (docs/0003) held the
hand-derived expectations for `for-in` and `switch`; both are promoted by this
chunk, and three new cases pin the parts they did not reach.

## Decision 1 — `for-in` materializes its keys, then walks them by index

`bronze_for_in_keys(obj)` returns a plain array of strings, and the loop is
the `lowerIndexWalkLoop` that `for-of` already uses (docs/0012 decision 3).
The alternative — a live cursor into the shape chain, resumed each iteration —
was rejected for three reasons, in increasing order of weight.

The small one is that the two loops then share their entire lowering: one
header/body/update/exit shape, one threaded index parameter, one set of
block-argument rules. `for-in` costs one runtime helper and one IL opcode
(`forin.keys`) rather than a second loop form.

The middling one is that a nullish operand falls out for free. ECMA-262
14.7.5.5 makes `for (const k in null)` a loop over nothing rather than a
TypeError — the one place in the language where a property operation on
`null` is not an error. A snapshot of `null` is an empty array, and an index
walk over an empty array runs zero times. A cursor would have needed the
special case written out.

The heavy one is mutation during enumeration. 14.7.5.6 (EnumerateObject
Properties) deliberately leaves the behaviour of a property **added** during
enumeration implementation-defined, while requiring that a property
**deleted** before it is visited is not visited, and that every key is visited
at most once. A snapshot pins the first: added keys never appear, in every
build, under every optimization level — which is the determinism rule of
CLAUDE.md applied to a place the spec declines to. It gets the third right by
construction. It gets the second wrong today only in the sense that bronze has
no `delete` (see `cases/blocked/delete_operator.js`); when dictionary mode
lands, the walk will have to re-check presence per key, and the snapshot is
the structure that makes that a one-line addition rather than a redesign.

The walk collects **own then inherited** keys, level by level up the
prototype chain, deduplicating by string equality against what earlier levels
already produced. Within a level the order is docs/0009's: integer-like keys
ascending, then string keys in insertion order. The chain walk is bounded by
`kMaxPrototypeDepth` so a cyclic prototype is a bounded error rather than a
hang.

## Decision 2 — enumerability is a bit on the shape node, and part of the transition key

A class method must not appear in `for-in`. ECMA-262 15.7.14 defines it with
`enumerable: false`, and before this chunk bronze defined it with
`prop.set` — an ordinary assignment, which always creates an enumerable
property. Nothing showed the difference until `for-in` existed, at which point
every instance of every class would have enumerated its methods.

So `Shape` gained `bool enumerable`, `addProperty` gained an `is_enumerable`
argument, and `ownKeysInInsertionOrder` gained an `enumerableOnly` filter that
`Object.keys`, object spread and `for-in` all pass. The IL gained
`method.def obj, <key>, v`, which is `prop.set` with the bit cleared and no
inline cache — a method is defined once, so there is no repeat to cache.

The part that is easy to get wrong: **the bit is part of the transition key,
not payload.** Every plain `{}` and every class prototype start from the same
root shape (docs/0008 decision 1). A transition matched on name alone would
let `class C { m() {} }` leave a non-enumerable `m` node on the root, which
the next `o.m = 1` on a fresh object would then reuse — and `Object.keys(o)`
would silently omit `m`. Matching on `(name, enumerable)` gives the two cases
different nodes.

## Decision 3 — one jump stack answers `break`, `continue`, and every label

`loopStack_` is now `jumpStack_`, and its entries carry a `JumpKind` of
`Loop`, `Switch` or `LabeledBlock` plus the label the statement was written
with (empty for most). Three search rules, in `findJumpTarget`:

- unlabelled `break` — the innermost `Loop` **or** `Switch`;
- unlabelled `continue` — the innermost `Loop`, skipping past any `Switch`;
- labelled either — the entry carrying that label, and for `continue` that
  entry must be a `Loop`.

The third rule is why a labelled block sits on the same stack as a loop: it is
a legal `break` target and an illegal `continue` one, and only one stack can
answer both questions in the same nesting order.

A label is not a binding, produces no value, and does not close over anything,
so `labelStack_` is saved, **cleared**, and restored across a function
boundary. `break outer` inside a nested function names nothing.

A statement that owns a jump target already builds an exit block, so labelling
it is nothing more than handing it the name: `lowerLabeledStmt` sets
`pendingLabel_` and the loop or switch consumes it with `takePendingLabel()`.
Anything else — a labelled plain block — gets an exit block of its own, with
one block parameter per variable the statement assigns, which is the same
shape a loop's exit block has and for the same reason.

## Decision 4 — `switch` is a test chain, then a body chain, matched with strict equality

Two chains of blocks, built separately and wired together.

The **test chain** evaluates the discriminant once (14.12.4), then one block
per `case` in source order, each holding that case's expression and a
`strict.eq` against the discriminant, branching to the matching clause's body
block or on to the next test. Case expressions are arbitrary expressions and
are evaluated **in order, only until one matches** — which is observable, and
is pinned in `switch_edges.js` by a probe function that never sees its third
call. The final test falls through to the `default` clause's body if there is
one and to the exit block if there is not.

The **body chain** is one block per clause, each falling through to the next
unless the clause jumps: fallthrough is the default (14.12.4 evaluates the
StatementList of every clause from the selected one onward), and `break` is
what stops it.

Matching is `strict.eq`, never `cmp.eq`. IsStrictlyEqual is not a style
choice here: it says `NaN` matches nothing including itself, so `case NaN:` is
dead code, and it says `+0` and `-0` are the same value, so `switch (-0)`
selects `case 0:`. Both are pinned.

Every block in both chains takes the same parameter list — one per variable
the switch assigns anywhere — because a body block can be entered from its
test, from the clause above it by fallthrough, or from neither, and the join
at the exit has to be an upper bound of all of them.

## Decision 5 — `default` keeps the position it was written in

`default` is not "the else". 14.12.4 walks the `case` list for a match first,
whatever the order, and only falls back to the default clause when no case
matched — but once the default clause is *entered*, execution continues into
the clauses **written after it**. A `default` in the middle that falls through
into `case 3:` is well-defined and is what the test chain's structure
naturally produces: the default's body block is at its written position in the
body chain, and only the *entry* into it is deferred to the end of the test
chain. `switch.js` pins a middle default falling through.

## Decision 6 — a lexical declaration directly in a case is a named error

This is a **deliberate deviation** from ECMA-262 and the one place this chunk
does less than the spec.

A switch body is one block scope (14.12.2 creates a single declarative
environment for the whole CaseBlock), so `let x` in one clause is in scope in
every clause, and in its temporal dead zone in the ones above it. Entering a
clause below the declaration by a case match and then reading `x` is a
`ReferenceError` — and bronze has no `throw`, so it cannot produce one. The
choices were to read `undefined` silently, which is a wrong answer of exactly
the kind docs/0000 exists to prevent, or to refuse.

It refuses, and the diagnostic names the fix:

> `unsupported construct: a 'let' declaration directly in a switch case (the
> switch body is one scope, so a case jump could reach it uninitialized); wrap
> the case body in a block`

Wrapping the clause in `{ }` gives the declaration a scope of its own and is
what most JavaScript writes anyway — it is what ESLint's `no-case-declarations`
has asked for since 2015. The blocked case for `try`/`catch`/`throw` carries
the mechanism that would lift this restriction; `lower_jumps_test.cpp` pins
both the error and the fact that the suggested fix compiles, so the diagnostic
cannot rot into a refusal that names a workaround that does not work.

`var` in a clause is accepted, because it has no dead zone.

## Decision 7 — an optional chain is an n-way join, not a per-link test

`a?.b?.c` has two places that can short-circuit and one place they both land.
Lowering it as "read, then test for undefined, then read" would be wrong twice
over: it would evaluate the rest of the chain, and it would not distinguish a
short circuit from a link that legitimately produced `undefined`.

So the chain is lowered as a spine. `lowerOptionalChain` walks it, each
optional link emits `is.nullish` and a branch into a fresh block that is
**recorded, not terminated**, and the success path continues. At the end, the
recorded short-circuit blocks and the success block are all edges into one
join block, which takes a `dynamic` parameter for the chain's result plus one
parameter per binding whose SSA value differs across the edges. That is the
docs/0005 block-argument join, widened from two edges to n.

Two details make it correct rather than nearly correct:

**Where the chain ends.** A `spinePos_` flag is set only for the base/callee
position of a link and consumed at the top of `lowerExpr`. So a `?.` written
inside an argument or inside an index expression starts its **own** chain, and
`a?.b(c?.d)` short-circuits the two independently: `outer?.b(nn?.d)` still
calls `b`, with `undefined`. Parentheses end a chain for the same reason —
`(a?.b).c` is a new MemberExpression whose base happens to be a parenthesized
optional expression, so `.c` is an ordinary access, and with a nullish `a` it
is a hard error rather than `undefined`.

**When the short circuit fires.** For `a?.[k]` the branch is emitted **before**
`k` is evaluated, and for `a?.(args)` before the arguments are. Skipping is
skipping. `optional_chaining.js` pins this with a counter: the key function is
called once across a hit and a miss.

## Decision 8 — an optional chain is not an assignment target

`a?.b = 1` and `a?.b++` are early errors:

> `an optional chain is not a valid assignment target`
> `an optional chain is not a valid target for '++' or '--'`

13.3.9's grammar simply does not admit an OptionalExpression as an
AssignmentTarget, and the reason is worth stating: there is no reference to
write through when the chain short-circuits, so the operation would have to
mean "sometimes write, sometimes silently do nothing". `a?.b` as a *read* is
still legal everywhere, including as the operand of `delete` (which bronze
does not have yet, and which the blocked case pins).

`` a?.`tag` `` is also rejected —
`a tagged template may not be part of an optional chain` — which is 13.3's
restriction, present because a short-circuited tag would have to produce a
template object nobody consumes.

## Decision 9 — reading a property of `null` or `undefined` is a hard error

Found while writing decision 7's test, and it is the reason `a?.b.c.d` is a
real test rather than a tautology. `bronze_prop_get` returned `undefined` for
a nullish receiver and `bronze_prop_set` discarded the write. ECMA-262 7.3.2
and 7.3.4 both begin with an assertion that the receiver is an Object; the
spec-visible behaviour is a TypeError.

With the old behaviour the whole of optional chaining was unobservable —
`a.b.c` and `a?.b?.c` would have printed the same thing for every input. Now
`reading property 'c' of undefined` is a named hard runtime error, `a?.b.c` is
the way to avoid it, and the difference is pinned.

## What this chunk does not do

- **No `throw`, so no TDZ.** Decision 6 above; and `(a?.b).c` with a nullish
  `a` aborts rather than throwing, which is why the oracle case pins only the
  non-nullish spelling of it. Both wait on `cases/blocked/try_catch_throw.js`.
- **No `delete`**, so the "deleted before it is visited" half of 14.7.5.6 is
  untested. `cases/blocked/delete_operator.js` holds the expectations.
- **`for (k in o)` with an existing binding** — a for-in/for-of head that
  assigns rather than declares — is a named error rather than a silent
  mis-scope: `unsupported construct: a for-in / for-of head that assigns an
  existing binding (write \`for (const x in o)\`)`.
- **Symbol keys** do not exist, so 14.7.5.6's restriction to String-valued
  keys is satisfied vacuously.

## Divergences from node

None deliberate in output. Two in acceptance: decision 6 rejects a program
node runs, and the for-in head above. Both are named errors, both are
mechanical to rewrite, and both are recorded here so that a future chunk
retires them rather than rediscovering them.

## Diagnostics this chunk introduces

Parse:

- `a switch may have only one 'default' clause`
- `expected 'case' or 'default' in a switch body`
- `a label may not front a declaration`
- `a label must front exactly one statement`
- `an optional chain is not a valid assignment target`
- `an optional chain is not a valid target for '++' or '--'`
- `a tagged template may not be part of an optional chain`
- ``unsupported construct: a for-in / for-of head that assigns an existing
  binding (write `for (const x in o)`)``
- `unsupported construct: object literal getter or setter (accessor properties
  are not implemented)`

Lower:

- `duplicate label 'L' (it is already in scope)`
- `continue label 'L' does not name an enclosing loop (only a loop can be
  continued)`
- `break label 'L' does not name an enclosing labelled statement`
- `continue label 'L' does not name an enclosing labelled statement`
- `break statement outside of a loop or switch`
- `continue statement outside of a loop`
- `unsupported construct: a 'let' declaration directly in a switch case (the
  switch body is one scope, so a case jump could reach it uninitialized); wrap
  the case body in a block`

Runtime:

- `reading property 'k' of null`
- `reading property 'k' of undefined`

Retired: `unsupported construct: switch statement`, `for-in loop`, and
`labeled break/continue`.

## Bugs this chunk uncovered in existing code

Every one of these was found by writing a test for something else, which is
the pattern docs/0000 predicted and the reason the oracle cases come first.

1. **`collectHoistedVarsIn` never descended into `ForOfStmt`.** A `var`
   declared in a for-of body had no function-level binding at all. Fixed while
   adding the same walk for `for-in`, `switch` and labelled statements.

2. **`types/flow.cpp` treated a for-of body as opaque.** `dispatch()` had no
   case for `ForOfStmt`, so a call written *only* inside a for-of body was
   invisible to the call-graph signature fixpoint of docs/0010 — an **unsound**
   proof, not a missed optimization: a callee could be inferred `number` while
   a string reached it. `keyedLoop` now walks the body for both loop forms.

3. **`valueToElementIndex` rejected a string index.** `arr[k]` where `k` is a
   string died with `computed index must be a number` — and `for (const k in
   arr)` produces exactly that, since array indices enumerate as strings. It
   now runs the integer-like-key test that docs/0009 already defines.

4. **Nullish property access was silent.** Decision 9.

5. **`lowerClosure` did not restore its saved state on the failure path.** An
   early `return std::nullopt` from a failed body left the *enclosing*
   lowering holding the nested function's binding map, its cleared jump stack
   and its cleared label stack. Unwinding past a labelled statement then
   popped an empty vector — an assertion in a debug build and undefined
   behaviour in a release one. The three loop lowerings had the same shape of
   leak on their body-failure path and are fixed with it: every one of them now
   unwinds on both paths and reports afterwards.

A sixth, confirmed and deliberately left alone as out of scope: a `var`
declared inside a nested **block** is not hoisted to a function-level binding,
so `function f(c) { if (c) { var m = 1; } return m; }` is `undefined variable:
m`. It is why `switch_edges.js` does not pin a `var` declared in a clause and
read after the switch.

## Files this chunk added

`src/lower/lower_switch.cpp`, `lower_label.cpp`, `lower_iter_loop.cpp` and
`lower_expr_chain.cpp` — one per construct, split out of `lower_control.cpp`
(724 lines and rising) along the seam each construct already was.
`src/types/operator_types.{h,cpp}` is a different seam in the same spirit,
pulled out of `flow.cpp` (1004 lines): *what each operator's result type is,
as a pure function of its operands'*, which lowering answers independently and
must agree with exactly (docs/0010 decision 8).
`src/runtime/rt_enumerate.cpp` holds the key snapshot of decision 1.
`tests/lower/lower_jumps_test.cpp` holds the early errors, which are about
edges rather than about types and did not belong in `lower_test.cpp`.
