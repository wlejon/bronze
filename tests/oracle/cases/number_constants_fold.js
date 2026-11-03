// Number's value properties are folded to constants at compile time when no
// statement can rebind the name `Number` (types/builtin_taint.h,
// types/math_builtins.h numberValueProperty). The table there must hold the
// runtime's exact doubles: every constant is printed and compared to the
// runtime's own value read through a computed key (never folded).
//
// The object `Number` itself is used freely here — called, passed, written to —
// none of which can change a non-writable, non-configurable value property.

const names = ["EPSILON", "MAX_SAFE_INTEGER", "MIN_SAFE_INTEGER", "MAX_VALUE", "MIN_VALUE",
               "POSITIVE_INFINITY", "NEGATIVE_INFINITY", "NaN"];
const folded = [Number.EPSILON, Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER,
                Number.MAX_VALUE, Number.MIN_VALUE, Number.POSITIVE_INFINITY,
                Number.NEGATIVE_INFINITY, Number.NaN];
for (let i = 0; i < names.length; i++) {
  const dyn = Number[names[i]];
  console.log(names[i], folded[i], dyn, Object.is(folded[i], dyn));
}

// In a loop the compiler types as f64, the shape `Quaternion.slerpFlat` has.
function nearZero(x) { return Math.abs(x) <= Number.EPSILON; }
let hits = 0;
for (let i = 0; i < 1000; i++) if (nearZero((i - 500) * 1e-18)) hits++;
console.log(hits, 1 + Number.EPSILON > 1, 1 + Number.EPSILON / 2 > 1);
console.log(Number.MAX_SAFE_INTEGER + 2, -Number.MAX_VALUE * 2, Number.MIN_VALUE / 2);

// The object escapes and is mutated; the constants stay what they are.
const N = Number;
N.custom = 7;
Number.EPSILON = 1;
console.log(N === Number, Number.custom, Number.EPSILON === N.EPSILON, Number("12") + 1);
try { Object.defineProperty(N, "EPSILON", { value: 3 }); console.log("redefined"); }
catch (e) { console.log(e.constructor.name, Number.EPSILON); }
const d = Object.getOwnPropertyDescriptor(Number, "EPSILON");
console.log(d.value === Number.EPSILON, d.writable, d.enumerable, d.configurable);

// A shadowing binding reads its own members.
function local() {
  const Number = { EPSILON: "eps", NaN: 5 };
  return [Number.EPSILON, Number.NaN];
}
console.log(local());
function param(Number) { return Number.MAX_VALUE; }
console.log(param({ MAX_VALUE: "m" }));

// A non-constant member keeps its dynamic read.
console.log(typeof Number.isFinite, Number.EPSILONX, Number["EPS" + "ILON"] === Number.EPSILON);
