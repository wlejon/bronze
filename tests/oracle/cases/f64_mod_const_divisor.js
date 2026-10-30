// `%` by a literal divisor. An integral dividend takes brass's inline integer
// remainder (float_rem_inline), and an integer remainder by a constant d >= 3
// that is not a power of two is a multiply by a magic number and shifts
// (div_magic.hpp), not an idiv / sdiv. These sit on that lowering: each
// divisor class (small, the int32 edges, past 32 bits, past 2^53, a multiple
// of a large power of two), dividends at 0 / -0, around +-d and +-2d, the
// int32 and int64 ends (-2^63 is an exact int64 and is taken; 2^63 is not),
// and the operands that must take the fmod call: NaN, +-Infinity,
// non-integral, huge. The divisors 0, -1 and a negative one keep the divide
// or the call, and are here as the edges of what is taken.

function f(x) { return Object.is(x, -0) ? '-0' : String(x); }

function m3(x) { return x % 3; }
function m7(x) { return x % 7; }
function m10(x) { return x % 10; }
function m1000(x) { return x % 1000; }
function m16807(x) { return x % 16807; }
function m2147483647(x) { return x % 2147483647; }
function m2147483649(x) { return x % 2147483649; }
function m4294967297(x) { return x % 4294967297; }
function m1000000007(x) { return x % 1000000007; }
function m9007199254740991(x) { return x % 9007199254740991; }
function m3p61(x) { return x % 6917529027641081856; }
function mneg7(x) { return x % -7; }
function mneg1(x) { return x % -1; }
function m0(x) { return x % 0; }

const fns = [
  [3, m3], [7, m7], [10, m10], [1000, m1000], [16807, m16807], [2147483647, m2147483647],
  [2147483649, m2147483649], [4294967297, m4294967297], [1000000007, m1000000007],
  [9007199254740991, m9007199254740991], [6917529027641081856, m3p61], [-7, mneg7], [-1, mneg1], [0, m0],
];

// Warm every function on small integers of both signs, as a tier-up sees.
let warm = 0;
for (let i = 0; i < 4000; i++) {
  for (const [, fn] of fns) {
    const r = fn(i - 2000);
    if (r === r) warm += r;
  }
}
console.log('warm', warm);

const fixed = [
  0, -0, 1, -1, 5, -5, 6, -6, 9, -9,
  2147483647, -2147483647, -2147483648, 2147483648, 4294967296, -4294967296, 4294967297,
  9007199254740991, -9007199254740991, 9007199254740992, -9007199254740992,
  1e18, -1e18, 2 ** 62, -(2 ** 62), -(2 ** 63), 2 ** 63, 1e300, -1e300,
  9.5, -9.5, 5e-324, NaN, Infinity, -Infinity,
];

for (const [d, fn] of fns) {
  const xs = fixed.slice();
  if (d !== 0) xs.push(d - 1, d, d + 1, -d, -(d - 1), -(d + 1), 2 * d, -2 * d, 2 * d - 1, -(2 * d - 1));
  console.log(String(d) + ':', xs.map((x) => f(fn(x))).join(' '));
}

// The bench's Park-Miller generator: the state lives in a closure, and its
// product reaches 2^45, so the remainder is an int64 one.
function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return s;
  };
}
const rng = makeLCG(12345);
const seen = [];
let sum = 0;
for (let i = 1; i <= 200000; i++) {
  const v = rng();
  sum = (sum + v) % 1000000007;
  if (i === 1 || i === 2 || i === 10 || i === 1000 || i === 200000) seen.push(v);
}
console.log('lcg', seen.join(' '), sum);

// A negative seed and one that is a multiple of the modulus.
const neg = makeLCG(-5);
const zero = makeLCG(2147483647 * 3);
console.log('seeds', neg(), neg(), zero(), zero());
