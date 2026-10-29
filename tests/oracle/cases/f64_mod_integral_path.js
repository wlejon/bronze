// `%` on doubles: an integer pair takes brass's inline integer remainder
// (float_rem_inline), everything else the fmod call. The cases sit on that
// seam: the dividend's sign on a zero result (-0), divisors 0 and -1, the
// ends of the int32 and int64 ranges, non-integral and non-finite operands.

function f(x) { return Object.is(x, -0) ? '-0' : String(x); }
function m(a, b) { return a % b; }

// Warm on small non-negative integers first, as a tier-up would see.
let warm = 0;
for (let i = 0; i < 5000; i++) warm += m(i, 7) + (i % 100) + ((i + 1) % 10);
console.log('warm', warm);

const pairs = [
  [-7, 3], [-6, 3], [7, -3], [-7, -3], [6, -3], [-0, 5], [0, -5], [-0, -5],
  [5, 0], [5, -0], [-5, 0], [0, 0], [5, -1], [-5, -1], [0, -1], [-0, -1],
  [-2147483648, -1], [-2147483648, 3], [2147483647, 2], [2147483648, 7], [-2147483649, 10],
  [2 ** 53, 3], [2 ** 53 + 2, 3], [1e18, 7], [-1e18, 7], [1e18, -1e17 - 3],
  [2 ** 63, 10], [-(2 ** 63), 10], [-(2 ** 63), -1], [-(2 ** 63), 2 ** 62], [10, 2 ** 63], [-10, -(2 ** 63)],
  [1e300, 7],
  [5.5, 2], [-5.5, 2], [5, 2.5], [5, 1.5], [-5, 1.5], [0.5, 1], [-0.5, 1],
  [NaN, 3], [3, NaN], [Infinity, 3], [-Infinity, 3], [3, Infinity], [-3, -Infinity], [-0, Infinity],
];
const out = [];
for (const [a, b] of pairs) out.push(f(m(a, b)));
console.log(out.join(' '));

// A literal divisor (the expansion's constant forms), negative dividends,
// across zero.
let line = [];
for (let i = -12; i <= 12; i++) line.push(f(i % 5));
console.log(line.join(' '));
line = [];
for (let i = -12; i <= 12; i++) line.push(f(i % -5));
console.log(line.join(' '));

// A variable divisor sweeping through -1, 0 and 1.
line = [];
for (let d = -3; d <= 3; d++) {
  for (let i = -4; i <= 4; i += 4) line.push(f(i % d));
}
console.log(line.join(' '));

// Park-Miller: the product leaves int32 but stays an exact integer.
let s = 12345;
let acc = 0;
for (let i = 0; i < 1000; i++) {
  s = (s * 16807) % 2147483647;
  acc = (acc + s) % 1000003;
}
console.log('lcg', s, acc);

// A remainder whose operand only sometimes is integral, in one loop.
let mixed = 0;
for (let i = 0; i < 64; i++) {
  const a = (i & 3) === 0 ? i + 0.25 : i;
  mixed += a % 6;
}
console.log('mixed', mixed);
