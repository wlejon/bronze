// `typeof` (docs/0015 decision 4). Six strings and no more: "undefined",
// "object" (which includes null, ECMA-262's oldest bug and one every engine
// reproduces), "boolean", "number", "string" and "function".
//
// bronze deliberately DIVERGES on one form: `typeof undeclaredName` is
// "undefined" in JavaScript, because typeof is the one operator that does
// not evaluate its operand as a reference. bronze resolves free identifiers
// at compile time against a closed set (docs/0011 decision 1), so an unknown
// name is a compile error there and cannot reach this operator - see
// docs/0015 decision 4 for why that is kept rather than special-cased.
console.log(typeof undefined);
console.log(typeof null);
console.log(typeof 0);
console.log(typeof 1.5);
console.log(typeof NaN);
console.log(typeof Infinity);
console.log(typeof true);
console.log(typeof false);
console.log(typeof "");
console.log(typeof "text");
console.log(typeof {});
console.log(typeof []);
console.log(typeof Math);
console.log(typeof Math.abs);

function named() {}
console.log(typeof named);
const arrow = (x) => x;
console.log(typeof arrow);
class C {
  constructor() {
    this.v = 1;
  }
  m() {
    return this.v;
  }
}
console.log(typeof C);
const c = new C();
console.log(typeof c);
console.log(typeof c.m);
console.log(typeof c.v);

// A declared-but-unassigned binding, and a property that is not there.
let u;
console.log(typeof u);
console.log(typeof c.missing);

// typeof always yields a string, so typeof of one is "string".
console.log(typeof typeof 1);

// Precedence: typeof is a unary operator, so it binds tighter than `+`.
// `typeof 1 + 1` is `(typeof 1) + 1`, which is string concatenation.
console.log(typeof 1 + 1);
console.log(typeof (1 + 1));
console.log(typeof 1 === "number");
console.log(typeof null === "object");

// The operand is evaluated, once, for its effects.
let calls = 0;
const bump = () => {
  calls = calls + 1;
  return 1;
};
console.log(typeof bump());
console.log(calls);
