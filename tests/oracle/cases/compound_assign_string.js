// `+=` is JS `+` (ECMA-262 13.15.3 -> ApplyStringOrNumericBinaryOperator):
// once ToPrimitive gives a String on either side the operator concatenates,
// and it is ToNumber addition only when neither side is a string. The other
// compound operators are ToNumber on both operands unconditionally.
//
// Lowering used to take the numeric path for EVERY compound assignment whose
// target was a plain identifier, so `s += "b"` unboxed a string pointer as a
// double. It now takes it only where inference proves the result is a Number;
// the member/index targets always went through the dynamic add and are pinned
// by compound_member_assign.js.

let s = "a";
s += "b";
console.log(s);

// Proven numeric: still a single f64 add, no boxing.
let n = 1;
n += 2;
console.log(n);
n += 0.5;
console.log(n);

// -=, *=, /=, %= are numeric whatever the operands were.
let m = 10;
m -= 4;
m *= 3;
m /= 2;
m %= 4;
console.log(m);

// Mixed operands: ToString on the number, then concatenation.
let label = "n=";
label += 5;
console.log(label);

let v = 7;
v += "!";
console.log(v);
v += 1;
console.log(v);

// The same two rules through an environment record. A captured binding is
// memory, not SSA, so it takes the other of lowering's two identifier paths —
// the one that had the identical defect.
function joiner() {
  let acc = "a";
  const add = function (t) { acc += t; };
  add("b");
  add("c");
  return acc;
}
console.log(joiner());

function counter() {
  let total = 0;
  const bump = function (k) { total += k; };
  bump(2);
  bump(3);
  return total;
}
console.log(counter());
