// `undefined` and `null` reaching a promoted chain. Neither is a Number — both
// tags sit above the number range — so both FAIL the guard and the original
// operator runs, which is 13.15.3 into 7.1.4 ToNumber: `null` is +0 and
// `undefined` is NaN (table 14).
//
// Chunk 1 has no widened guard, so this case is the measurement that says
// whether one would pay: what it pins today is that the ordinary path is still
// exactly right, including the sign of the zero, which is the one thing a
// branchless coercion could get wrong.

function sumOne(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a;
  }
  return total;
}

// ToNumber(null) is +0, so the sum stays +0 and `1 / x` says which zero.
const nulls = sumOne({ a: null }, 5);
console.log(nulls);
console.log(Object.is(nulls, 0));
console.log(1 / nulls);

// ToNumber(undefined) is NaN, and NaN is sticky.
console.log(sumOne({ a: undefined }, 5));
// A missing property is `undefined` by the same rule.
console.log(sumOne({ b: 1 }, 5));

// A -0 accumulator meeting +0: 6.1.6.1.7 gives +0 when the signs differ.
function fromNegZero(o, n) {
  let total = -0;
  for (let i = 0; i < n; i++) {
    total = total + o.a;
  }
  return total;
}
const mixedZero = fromNegZero({ a: null }, 3);
console.log(Object.is(mixedZero, -0));
console.log(1 / mixedZero);
// Nothing added at all leaves the -0 the way it came in.
console.log(1 / fromNegZero({ a: null }, 0));

// A property that is a Number and then becomes nullish: the prefix is real
// arithmetic and the rest is not.
function flip(o, at, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    if (i === at) {
      o.a = null;
    }
    total = total + o.a;
  }
  return total;
}
console.log(flip({ a: 2 }, 3, 6));
console.log(flip({ a: 2 }, 0, 6));

function flipUndef(o, at, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    if (i === at) {
      o.a = undefined;
    }
    total = total + o.a;
  }
  return total;
}
console.log(flipUndef({ a: 2 }, 3, 6));
