// The whole binding ladder, one line per rung (docs/0015 decision 10).
// Each line is chosen so that getting the rung wrong prints something
// DIFFERENT, not merely something equal by luck: a case that reads the same
// under both groupings pins nothing.
//
// Lowest to highest: comma, assignment, conditional, ?? / || , &&, |, ^, &,
// equality, relational (< > <= >= in instanceof), shift, additive,
// multiplicative, **, unary, postfix, call/member.

// comma is looser than assignment: `(x = 1, 2)` assigns 1 and yields 2.
let x = 0;
const commaValue = (x = 1, 2);
console.log(x);
console.log(commaValue);

// assignment is looser than the conditional, and right-associative. Parsed
// the other way round, `x = false ? 1 : 2` would assign `false` to x and
// throw the 2 away.
x = false ? 1 : 2;
console.log(x);
let a = 0;
let b = 0;
a = b = 3;
console.log(a);
console.log(b);

// || is looser than &&: `true || false && false` keeps the true.
console.log(true || false && false);
// && is looser than |: `0 && 1 | 1` short-circuits to 0, where the other
// grouping would compute (0 && 1) | 1 and print 1.
console.log(0 && 1 | 1);
// | is looser than ^, which is looser than &.
console.log(1 | 2 ^ 3);
console.log(1 ^ 3 & 2);
// & is looser than equality: `1 & 1 == 1` is `1 & true`, which is 1.
console.log(1 & 1 == 1);
// equality is looser than relational.
console.log(1 < 2 == true);
// relational is looser than shift.
console.log(1 << 2 < 5);
// shift is looser than additive.
console.log(1 << 1 + 1);
// additive is looser than multiplicative.
console.log(1 + 2 * 3);
// multiplicative is looser than **.
console.log(2 * 3 ** 2);
// ** is looser than unary on its RIGHT (its left operand may not be one).
console.log(2 ** -2);
// unary is looser than call and member access.
const obj = { n: 5, f: () => 7 };
console.log(typeof obj.n);
console.log(-obj.f());
console.log(!obj.n);
console.log(~obj.n);
