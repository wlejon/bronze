# 0015 — The rest of the operators

Status: implemented. Part of phase 4 of docs/0001.

`console.log(5 & 3)` reported `unrecognized character '&'`. So did every
other operator in this document: the bitwise family and its shifts, `**`,
`typeof`, `instanceof`, `in`, `void`, the comma operator, and loose
equality. Between them they are most of the ECMA-262 operator table, and
their absence is not a missing convenience — `x | 0` is how idiomatic JS
says "truncate to int32", `typeof x === "string"` is how it asks a
question about an untyped value, and three.js uses both on nearly every
page.

Two of them were worse than absent. `==` and `!=` reached lowering and
died there with a named error, which is the honest failure; the third
named error at the same site, "mixed i32/f64 strict equality", was a hole
in `===` itself and is closed here.

## Decision 1 — the bitwise family is int32 *inside* the operator and a number outside it

ECMA-262 defines `a & b` as ToInt32 on both operands, an integer
operation, and a result converted back to a Number. Every part of that is
observable: `2147483648 | 0` is `-2147483648` because the conversion is
modulo 2^32, `NaN | 0` is `0`, and `-1 >>> 0` is `4294967295` because
`>>>` — alone in the family — converts its result as *unsigned*.

The IL says exactly this. `to.int32` produces an `i32`, the six bitwise
ops take `i32` operands, and their *result type is `f64`*:

```
%3: i32 = to.int32 %1
%4: i32 = to.int32 %2
%5: f64 = and %3, %4
```

The result could have been an `i32`, and that would have been faster to
read. It would also have been unsound: `src/types` has a flat lattice
with a single `Number` element and no int32 (docs/0010), so an `i32`
escaping into a block parameter, a call signature or a `let` would give
inference and the backend two different stories about the same value.
Keeping the integer strictly *inside* the operator means the widening is
part of the operator's definition rather than a coercion at its use site,
and a chain like `(a | b) & c` still costs one conversion per *source*
operand rather than one per operator — the `to.int32` in front of an
already-`i32` value is a no-op in the backend.

`~x` is `xor(to.int32 x, const.i32 -1)` rather than an op of its own.
There is exactly one bit pattern that inverts every bit, the IL already
had `xor`, and a `BitNot` op would have been a second spelling of the
same instruction that a peephole would have to learn to fold.

## Decision 2 — ToInt32 is a call, and the shift count is masked in the backend

`fptosi` is poison for a double outside the int32 range. The language
requires a defined answer there — a wraparound — so the conversion cannot
be a cast and lives in `bronze_to_int32_f64`, with `bronze_to_int32` in
front of it for a boxed operand (which is also where a string operand
goes through ToNumber, so `"12" & 10` is `8`).

Shifts have the mirror-image problem. LLVM says a shift by ≥ the bit
width is poison; JS says `1 << 32` is `1`. The backend masks the count
with `& 31` unconditionally, which is the spec's `ToUint32(rhs) & 31` —
the mask is over the same 32 bits ToInt32 produced, so ToUint32 and
ToInt32 cannot disagree about the low five and one conversion serves
both.

## Decision 3 — `**` is right-associative, and its left unary operand is an error

`2 ** 3 ** 2` is `512`. In a precedence-climbing parser that is one line:
the right operand is parsed at the operator's *own* precedence instead of
one above it, and every other binary operator keeps the `+ 1`.

`-2 ** 2` is a SyntaxError in ECMA-262, and bronze reports it rather than
choosing. Both readings — `(-2) ** 2` is `4`, `-(2 ** 2)` is `-4` — are
things a programmer plausibly meant, and the committee's judgement that
neither can be assumed applies to a compiler at least as strongly as to
an interpreter. The check needs to know whether the left operand was
*parenthesized*, which is why `ast::Expr` grew a `parenthesized` flag;
`(-2) ** 2` compiles.

`**` and `Math.pow` share one `rtExponentiate`, because they are the same
function in the spec and would otherwise drift. Neither is C's `pow`:
`std::pow(1, NaN)` is `1` and `std::pow(-1, inf)` is `1`, while
Number::exponentiate says both are NaN, and a program that tests `x ** y`
for NaN can tell the difference.

## Decision 4 — `typeof` on an undeclared name is a deliberate divergence

In JavaScript, `typeof undeclaredName` is the one expression that reads an
unbound identifier without throwing: it evaluates to `"undefined"`. That
is the idiom every feature-detection line is written in.

bronze cannot do it, and will not pretend to. An unknown identifier is a
compile-time error here — there is no global object to miss in, and
resolving names at compile time is what the whole inference design rests
on (docs/0010). Answering `"undefined"` for a name that is simply
misspelled would turn a diagnosable typo into a silent wrong branch,
which docs/0000 names as the failure mode worth the most vigilance.

So the divergence is recorded rather than hidden: **`typeof x` for an
undeclared `x` is `unknown identifier` at compile time, not
`"undefined"`.** Every other `typeof` is spec-exact, including
`typeof null === "object"` — ECMA-262's oldest wart, kept because every
engine keeps it and real programs test for it.

The six result strings are allocated once and permanently rooted. A fresh
heap string per evaluation would put an allocation, and therefore a
possible collection, inside an operator that cannot fail.

## Decision 5 — `instanceof` and `in` are prototype-chain walks, and their type errors are named

`instanceof` materializes the right operand's `.prototype` and then walks
the left operand's proto chain comparing object identity at each link.
Materializing first is load-bearing: a constructor whose `.prototype` has
never been read has no prototype object yet (docs/0008), and creating a
fresh one per test would make a constructor answer `false` for its own
instances.

A primitive on the *left* is `false`, not an error — that is what makes
`x instanceof C` a safe guard on an unknown value. A non-callable on the
right is a TypeError in the spec and a named hard error here.

`in` is the same walk minus the part that reads the value, which is
exactly the difference between `"k" in o` and `o.k`: a property whose
value is `undefined` still answers `true`. Arrays answer for `length` and
for indices *within* the length, which is the reason `in` exists on an
array at all. A non-object on the right is a named hard error.

## Decision 6 — comma is at the bottom of the ladder, and almost nothing calls it

The full precedence ladder, lowest first, is now: comma, assignment,
conditional, `??`/`||`, `&&`, `|`, `^`, `&`, equality, relational
(including `in` and `instanceof`), shift, additive, multiplicative, `**`,
unary, postfix, call/member.

The hazard in adding the bottom rung is that the grammar's other users do
not want it. Function arguments, array elements, object property values
and variable initializers are *AssignmentExpression*, not *Expression* —
if they call the comma parser, `f(a, b)` quietly becomes a one-argument
call. Those four positions were switched to `parseAssign` in the same
change, and `operators_misc` pins all four against the failure by
printing something different if it happens.

Wiring the ladder up correctly also fixed two things that were already
wrong, described in decision 8.

## Decision 7 — loose equality is a runtime algorithm, in the spec's order

`bronze_loose_eq` is ECMA-262 7.2.14 written out step by step. The order
*is* the specification: `null == 0` is false only because the nullish
step answers before any ToNumber can run, and any reordering of the
coercions makes it true. Same-type operands fall through to strict
equality; a boolean operand is ToNumber'd and the comparison restarts; a
number against a string converts the *string*, never the number.

The one case bronze cannot finish is an object against a primitive, which
needs ToPrimitive — `valueOf` then `toString`, neither of which bronze
has an `Object.prototype` to find. It is a named hard error, on the same
rule as ToString of an object.

When lowering can prove both operands are the same unboxed type, `==`
lowers to the same compare `===` does. That is not an optimization on top
of the algorithm; it is the algorithm's first step, and it is what makes
`==` in typed code cost nothing.

## Decision 8 — the two parser bugs the ladder uncovered

Assignment was a left-associative binary operator at precedence 0, below
the ternary. Two consequences, both silent wrong answers rather than
errors:

- `x = cond ? a : b` parsed as `(x = cond) ? a : b`. The variable got the
  *condition*.
- `a = b = 3` parsed as `(a = b) = 3`.

Splitting `parseAssign` (right-associative, above comma) from
`parseConditional` (whose arms are AssignmentExpressions) is the shape
the spec's grammar already had, and both lines are now pinned in
`operators_precedence`.

Separately, `??` could be mixed with `&&` or `||` without parentheses.
ECMA-262 makes that a SyntaxError — `a ?? b || c` has no agreed reading —
and bronze now says so. The check is on the *unparenthesized* form, so
`(a ?? b) || c` compiles, which is the same `Expr::parenthesized` flag
decision 3 needed.

## Decision 9 — `cmp.ne` is the negation of `cmp.eq`; numeric truthiness gets its own op

The `loose_equality` case asked what `NaN !== NaN` prints. It printed
`false`.

The cause predates this work. `il::Op::CmpNe` on doubles was emitted as
LLVM's *ordered* `fcmp one`, which answers false when either operand is
NaN. That is the right instruction for numeric truthiness — `if (NaN)`
must be falsy, so NaN must not count as "not equal to 0" — and the wrong
one for `!==`, which is the exact negation of `===` and must answer true.
One op was doing both jobs, and they differ at precisely one value.

`cmp.ne` is now the unordered compare, i.e. the true negation of
`cmp.eq`, and ToBoolean-of-a-number is a separate op, `num.truthy`,
carrying the ordered form. Naming it is the point: every use of one is a
wrong answer for the other, and a comment saying "this `cmp.ne` means the
other thing" would have been one refactor away from the same bug.

## Named diagnostics

- `'**' cannot have an unparenthesized unary operand on its left (write
  (-x) ** y or -(x ** y))` — decision 3.
- `'??' cannot be mixed with '&&' or '||' without parentheses` —
  decision 8.
- `the right operand of 'instanceof' is not callable` — decision 5.
- `the right operand of 'in' must be an object` — decision 5.
- `'==' between an object and a primitive needs ToPrimitive, which is
  unsupported` — decision 7.
- `typeof on a symbol is unsupported (bronze has no symbols)` — the sixth
  `typeof` string is reachable only through a value bronze cannot build,
  so the branch names what is missing instead of falling through to
  `"object"`.

## What is deliberately not here

- **`ToPrimitive`.** Decision 7's hard error, and the same gap behind
  `String(obj)`. It needs `Object.prototype` with real `valueOf` and
  `toString` methods, which is a builtins question (docs/0011), not an
  operator one.
- **`delete`, `await`, `yield`.** Not operator arithmetic; each needs a
  semantics of its own.
- **BigInt.** Every operator here is specified twice in ECMA-262, once
  for Number and once for BigInt. bronze has no BigInt, so it implements
  the Number half and the other half cannot be reached.
