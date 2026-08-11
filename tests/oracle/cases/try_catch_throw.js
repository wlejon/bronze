// The shape of a non-local exit (docs/0020). What this case pins:
//
// 1. A throw crosses a call boundary: `risky(-1)` leaves `risky` and lands
//    in the caller's `catch`, and the statements after it in the `try` do
//    not run.
// 2. `finally` runs on BOTH paths, including the one where `try` returns —
//    `f()` prints "f" before its value reaches the caller (ECMA-262 14.15.3
//    makes the finally completion win only if it is abrupt).
// 3. A thrown value is any value, not a special error type: an object here
//    keeps its properties.
// 4. Rethrowing from a `catch` propagates outward, so a two-level chain is
//    caught by the outermost handler.
function risky(n) {
  if (n < 0) {
    throw "negative";
  }
  return n * 2;
}
try {
  console.log(risky(2));
  console.log(risky(-1));
  console.log("unreached");
} catch (e) {
  console.log("caught " + e);
} finally {
  console.log("finally");
}
try {
  throw { code: 7 };
} catch (e) {
  console.log(e.code);
}
function f() {
  try {
    return "t";
  } finally {
    console.log("f");
  }
}
console.log(f());
function inner() {
  throw "inner";
}
function outer() {
  try {
    inner();
  } catch (e) {
    throw "wrapped:" + e;
  }
}
try {
  outer();
} catch (e) {
  console.log(e);
}
console.log("end");
