// `void`, the comma operator, and `**`.
//
// `void x` evaluates x for its effects and yields undefined - the whole of
// the operator. The comma operator evaluates its left operand for effects
// and yields its right; the hazard it carries is that function arguments,
// array elements and variable initializers are AssignmentExpression, not
// Expression, so a comma there is a separator and never this operator, which
// the calls below pin. `**` is right-associative, unlike every other binary
// operator in the language.
console.log(void 0);
console.log(void "x");
console.log(void {});

let n = 0;
console.log(void (n = 5));
console.log(n);

// The comma operator, where a comma really is one.
let c = 0;
const r = (c = 1, c + 1);
console.log(r);
console.log(c);
console.log((1, 2, 3));

let seen = "";
const mark = (s) => {
  seen = seen + s;
  return s;
};
const last = (mark("a"), mark("b"), mark("c"));
console.log(last);
console.log(seen);

// The commas that are NOT the operator. If the comma were wired in at the
// wrong precedence level, `two(1, 2)` would be a one-argument call and print
// undefined, `[1, 2, 3]` would be a one-element array, and the object below
// would have one property.
function two(a, b) {
  return b;
}
console.log(two(1, 2));
const arr = [1, 2, 3];
console.log(arr.length);
console.log(arr[2]);
const obj = { a: 1, b: 2 };
console.log(obj.b);
// A ternary's branches are AssignmentExpression too, so the comma below
// belongs to the argument list and not to the conditional.
console.log(two(true ? 1 : 2, 9));

// A `for` header is one place a real comma operator is idiomatic.
let i = 0;
let j = 0;
let sum = 0;
for (i = 0, j = 10; i < 3; i++, j--) {
  sum = sum + i + j;
}
console.log(sum);

// `**` binds tighter than `*` and looser than unary, and is the one
// right-associative binary operator: 2 ** 3 ** 2 is 2 ** 9.
console.log(2 ** 3);
console.log(2 ** 3 ** 2);
console.log((2 ** 3) ** 2);
console.log(2 * 3 ** 2);
console.log(2 ** 3 * 2);
console.log(2 ** -1);
console.log((-2) ** 2);
console.log(-(2 ** 2));
console.log(4 ** 0.5);
console.log(2 ** 0);
// ECMA-262 Number::exponentiate is not C's pow: an exponent of NaN is NaN
// even for base 1, and a base of magnitude 1 with an infinite exponent is
// NaN. A NaN base with a zero exponent is still 1.
console.log(1 ** NaN);
console.log(NaN ** 0);
console.log((-1) ** Infinity);
console.log(1 ** Infinity);

let e = 3;
e **= 2;
console.log(e);
e **= 0.5;
console.log(e);
