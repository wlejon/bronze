// `arguments.callee`, both of its mode-split answers (ECMA-262 10.2.4,
// 10.2.11 step 6, 10.4.4, 10.2.12).
//
// In sloppy code `callee` is a data property of the arguments object whose
// value is the function currently executing (10.4.4, 10.2.12), so it is `===`
// the binding that names it — for a declaration, and for a function
// EXPRESSION, which is the case the feature was ever really used for. It is a
// real property, not a magic read, so it survives being handed around, and it
// works as the recursion handle it historically was.
//
// In strict code the property is an accessor whose get and set halves are both
// %ThrowTypeError% (10.2.4) — the "poison pill" — so merely READING it throws
// a TypeError.
//
// An arrow has no `arguments` of its own, so `arguments.callee` inside one
// answers for the ENCLOSING function, not the arrow.

function named() {
  return arguments.callee;
}
console.log(named() === named);

const expr = function () {
  return arguments.callee;
};
console.log(expr() === expr);

function recurse(n) {
  if (n === 0) return 0;
  return n + arguments.callee(n - 1);
}
console.log(recurse(4));

function strictCallee() {
  "use strict";
  try {
    return arguments.callee;
  } catch (e) {
    return e instanceof TypeError ? e.name : "wrong error";
  }
}
console.log(strictCallee());

function outerCallee() {
  const inner = () => arguments.callee;
  return inner() === outerCallee;
}
console.log(outerCallee());
