// The fast path of a guarded numeric region, taken for every iteration
// (src/lower/guard_region.h). Nothing here should ever leave the fast copy, so
// what these lines pin is that the promoted `f64` arithmetic is ECMA-262
// Number::add and not an approximation of it: the sign of zero, the infinities,
// the tie at 2**53 and the subnormals all have to come out of an `fadd` exactly
// as they came out of the boxed operator.
//
// Printed through `String`, `Object.is` and `1 / x` because those are the three
// ways a Number's identity is observable: the decimal, the -0/+0 distinction
// that `===` cannot see, and the sign of an infinity.

function sumProps(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}

// 100000 iterations of +1 +2.
console.log(sumProps({ a: 1, b: 2 }, 100000));

// -0. 6.1.6.1.7 Number::add: -0 + -0 is -0, and +0 + -0 is +0.
function sumZeros(o, n) {
  let total = -0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
const negZeros = sumZeros({ a: -0, b: -0 }, 1000);
console.log(String(negZeros));
console.log(Object.is(negZeros, -0));
console.log(1 / negZeros);

const mixedZeros = sumZeros({ a: 0, b: -0 }, 1000);
console.log(Object.is(mixedZeros, 0));
console.log(1 / mixedZeros);

// The infinities. x + Infinity is Infinity; Infinity + -Infinity is NaN, and
// NaN is sticky from there.
console.log(sumProps({ a: Infinity, b: 1 }, 10));
console.log(sumProps({ a: -Infinity, b: -1 }, 10));
console.log(sumProps({ a: Infinity, b: -Infinity }, 10));

// The tie at 2**53. The exact sum 2**53 + 1 is halfway between two
// representable numbers and round-half-to-even takes the one with the even
// significand, which is 2**53 itself — so this loop never moves.
function fromBig(o, n) {
  let total = 9007199254740992;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
console.log(fromBig({ a: 1, b: 0 }, 100));
// The spacing at 2**53 is 2, so an addend of 2 is exact and this one moves by
// exactly 4 per iteration.
console.log(fromBig({ a: 2, b: 2 }, 4) === 9007199254740992 + 16);

// Subnormals. Every subnormal is an exact multiple of Number.MIN_VALUE, so the
// sum is exact and the ratio is an integer.
function fromZero(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
const tiny = fromZero({ a: 5e-324, b: 5e-324 }, 3);
console.log(tiny / 5e-324);
console.log(tiny > 0);
console.log(tiny < Number.MIN_VALUE * 7);

// A 17-significant-digit result, which is where a decimal that is not shortest
// round-trippable would show.
console.log(fromZero({ a: 0.1, b: 0.2 }, 1));
