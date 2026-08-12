// ECMA-262 13.15.2 (AssignmentExpression : LeftHandSideExpression
// AssignmentOperator AssignmentExpression): the target reference is
// evaluated first, its value is read (GetValue) SECOND, and only then is
// the right-hand side evaluated. The store (PutValue) is last.
//
// Every case below has a right-hand side that writes the target, so the two
// orders give different answers. Lowering evaluated the right-hand side first
// for an identifier target, which is invisible while the read emits no
// instruction and observable the moment it does — a captured binding is memory,
// so its read is an `env.get` that moved with the code.

let x = 1;
const bumpX = function () {
  x = 100;
  return 10;
};
x += bumpX();
console.log(x);

// The same rule for the string path: `+=` is JS `+`, so the left operand
// read has to be the pre-call one here too.
let s = "a";
const bumpS = function () {
  s = "Z";
  return "b";
};
s += bumpS();
console.log(s);

// Not just `+=`: every compound operator reads the target before the
// right-hand side.
let n = 50;
const bumpN = function () {
  n = 1;
  return 5;
};
n -= bumpN();
console.log(n);

// No closure in sight: an SSA-backed binding whose right-hand side is
// itself an assignment to it. The read is still first, so the inner
// assignment's value reaches only the right operand.
let y = 1;
y += (y = 3);
console.log(y);

// A member target has always read before the right-hand side; pinned so
// that it stays that way.
const o = { v: 1 };
const bumpO = function () {
  o.v = 100;
  return 10;
};
o.v += bumpO();
console.log(o.v);
