// The guard that fails on the SECOND of two operands in one iteration. The
// first add has already run, as an `fadd`, on values that were never boxed —
// so what reaches the String concatenation is a double that has to be re-boxed
// and then converted by 6.1.6.1.20 Number::toString exactly as the original
// operator would have converted it (src/lower/guard_region.h).
//
// -0 and a 17-significant-digit value are the two places a re-box could be
// caught lying: `String(-0)` is "0" and not "-0", and a shortest-round-trip
// decimal is the only correct rendering of a double that has no short one.

function run(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}

// Every iteration: a Number add, then a concatenation.
console.log(run({ a: 1, b: "b" }, 3));

// The partial sum is -0, which prints as "0".
function fromNegZero(o, n) {
  let total = -0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
console.log(fromNegZero({ a: -0, b: "!" }, 1));

// The partial sum needs all seventeen digits.
function fromTenth(o, n) {
  let total = 0.1;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
console.log(fromTenth({ a: 0.2, b: "=" }, 1));

// The partial sum is an infinity and a NaN.
console.log(run({ a: Infinity, b: "|" }, 1));
console.log(run({ a: NaN, b: "|" }, 1));

// The second operand is an object with a `valueOf`, so the guard fails and the
// SLOW copy runs 13.15.3 — which asks ToPrimitive with no hint, tries
// `valueOf`, gets a Number, and adds rather than concatenates.
const coercible = { valueOf() { return 5; } };
console.log(run({ a: 1, b: coercible }, 3));

// The second operand is a BigInt, so the slow copy reaches 13.15.3's mixing
// TypeError with the first add already done.
function caught(o) {
  try {
    return String(run(o, 1));
  } catch (e) {
    return e instanceof TypeError ? "TypeError" : e.constructor.name;
  }
}
console.log(caught({ a: 1, b: 2n }));
