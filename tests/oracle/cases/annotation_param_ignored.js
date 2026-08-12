// The live unsoundness: `function f(x: number)` reached with a string used to
// map straight onto an f64 parameter, so the call unboxed a string as a double.
// An annotation is a hint, and a hint no proof backs is discarded — the program
// below is wild JS and must run as wild JS.
//
// Two shapes of "no proof":
//   add1 is direct-callable, but its call sites join Number with String, so
//   the parameter is dynamic by JOIN — inference proved the opposite of the
//   annotation.
//   twice is exported, so it has callers outside this compilation and its
//   parameter can never be proven at all.
function add1(x: number) {
  return x + 1;
}

export function twice(s: number) {
  return s + s;
}

// ECMA-262 13.15.3 -> ApplyStringOrNumericBinaryOperator: `+` concatenates
// as soon as ToPrimitive gives a String on either side, and is ToNumber
// addition only when neither side is one.
console.log(add1(41));
console.log(add1("a"));
console.log(twice("ab"));
console.log(twice(21));
