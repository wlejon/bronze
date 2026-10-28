// Pristine Math's value properties are folded to constants at compile time
// (types/math_builtins.h mathValueProperty, lower_object.cpp). The table there
// must hold the runtime's exact doubles: every constant is printed, compared
// to the runtime's own value read through a computed key (never folded), and
// used in arithmetic whose bits would show a one-ulp difference.

const names = ["E", "LN10", "LN2", "LOG10E", "LOG2E", "PI", "SQRT1_2", "SQRT2"];
const folded = [Math.E, Math.LN10, Math.LN2, Math.LOG10E, Math.LOG2E, Math.PI,
                Math.SQRT1_2, Math.SQRT2];
for (let i = 0; i < names.length; i++) {
  const dyn = Math[names[i]];
  console.log(names[i], folded[i], dyn, Object.is(folded[i], dyn));
}

// Arithmetic on the folds, in a loop the compiler types as f64.
function circle(r) { return 2 * Math.PI * r; }
let acc = 0;
for (let i = 0; i < 1000; i++) acc += circle(i * 0.001) + Math.E * Math.SQRT2 - Math.LN2 / Math.LOG2E;
console.log(acc);
console.log(Math.PI * 1e300 * 1e10, -Math.PI / 0, Math.SQRT1_2 * Math.SQRT1_2);

// A fold is a read, not a binding: its descriptor is still the runtime's.
const d = Object.getOwnPropertyDescriptor(Math, "PI");
console.log(d.value === Math.PI, d.writable, d.enumerable, d.configurable);

// A non-constant member keeps its dynamic read.
console.log(typeof Math.max, Math.toString(), Math.PIE, Math["P" + "I"] === Math.PI);

// Writes to a constant do nothing (non-writable); in strict code they throw.
// The class body is strict.
class Writer { static tryWrite() { try { Math.PI = 3; return "wrote"; } catch (e) { return e.constructor.name; } } }
console.log(Writer.tryWrite(), Math.PI);
