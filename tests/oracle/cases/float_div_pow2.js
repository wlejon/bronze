// A division by a power-of-two constant, which brass's fdiv_pow2 answers as a
// multiplication by the reciprocal (`x / 2` as `x * 0.5`): the same IEEE
// operation for every x, so every quotient is printed as its exact bits and
// must match the division's. The divisors cover the rewritten ones (2, 0.5, -4,
// 1, 1024, 2^-1022, 2^1022), the ones left alone (2^1023, whose reciprocal is
// subnormal; 3; 0.1; -0; Infinity), and the dividends every edge of the
// rounding: subnormals, the largest double, the specials, odd mantissas.

const f64 = new Float64Array(1);
const u32 = new Uint32Array(f64.buffer);
function bits(x) {
  // A NaN's bits in a Float64Array are the implementation's choice (ECMA-262
  // NumericToRawBytes): only that it is a NaN is printed.
  if (x !== x) return 'NaN';
  f64[0] = x;
  return u32[1].toString(16).padStart(8, '0') + u32[0].toString(16).padStart(8, '0');
}

const inputs = [
  0, -0, 5e-324, -5e-324, 1e-323, 2.2250738585072014e-308, 2.225073858507201e-308, 4.4501477170144023e-308,
  1e-300, 0.1, 1, -1.5, 3, 7, 1e300, 8.98846567431158e307, 1.7976931348623157e308, -1.7976931348623157e308,
  4503599627370497, 9007199254740993, 0.30000000000000004, 1 / 3, Infinity, -Infinity, NaN,
];

// Each divisor is a literal at its own site: the rewrite needs the constant.
function byTwo(x) { return x / 2; }
function byHalf(x) { return x / 0.5; }
function byMinusFour(x) { return x / -4; }
function byOne(x) { return x / 1; }
function by1024(x) { return x / 1024; }
function byTiny(x) { return x / 2.2250738585072014e-308; }
function byHuge(x) { return x / 4.49423283715579e307; }
function byTop(x) { return x / 8.98846567431158e307; }
function byThree(x) { return x / 3; }
function byTenth(x) { return x / 0.1; }
function byMinusZero(x) { return x / -0; }
function byInfinity(x) { return x / Infinity; }

const kernels = [byTwo, byHalf, byMinusFour, byOne, by1024, byTiny, byHuge, byTop, byThree, byTenth, byMinusZero,
  byInfinity];

// Typed inputs (a Float64Array read is a proven double) and dynamic ones (an
// array of mixed values, so the division is the generic one).
const typed = Float64Array.from(inputs);
for (const k of kernels) {
  const row = [];
  for (let i = 0; i < typed.length; i++) row.push(bits(k(typed[i])));
  console.log(k.name, row.join(' '));
}

// A hot loop, so every tier compiles the division with the operand proven.
for (const k of [byTwo, byHalf, byMinusFour, byHuge]) {
  let acc = 0;
  let last = 0;
  for (let r = 0; r < 3000; r++) {
    for (let i = 0; i < typed.length; i++) {
      const q = k(typed[i]);
      if (q === q && Math.abs(q) !== Infinity) acc += q;
      last = q;
    }
  }
  console.log('loop', k.name, bits(acc), bits(last));
}

// Inline in a loop body rather than through a call.
{
  let s = 0;
  let t = 0;
  for (let i = 0; i < 20000; i++) {
    const x = (i - 10000) * 1.000000119;
    s += x / 2;
    t += x / 0.25 - x / 8;
  }
  console.log('inline', bits(s), bits(t));
}

// Mixed values: strings, booleans, null, undefined, objects with valueOf.
const mixed = ['8', '', ' 3 ', 'x', true, false, null, undefined, { valueOf() { return 6; } }, [4], [], 10n];
for (const v of mixed) {
  let out;
  try {
    out = bits(byTwo(v)) + ' ' + bits(byHalf(v));
  } catch (e) {
    out = e.constructor.name;
  }
  console.log('mixed', typeof v, out);
}

// Float32Array: a float32 quotient stored back rounds the same either way.
const f32 = new Float32Array([1, 3, 0.1, 3.4028234663852886e38, 1.401298464324817e-45, -0, NaN, Infinity]);
const o32 = new Float32Array(f32.length);
for (let r = 0; r < 2000; r++) {
  for (let i = 0; i < f32.length; i++) o32[i] = f32[i] / 2 + Math.fround(f32[i] / 4);
}
console.log('f32', Array.from(o32, bits).join(' '));
console.log('fround', bits(Math.fround(Math.fround(0.1) / 2)), bits(Math.fround(3.4028234663852886e38) / 0.5));

// The rewritten division keeps the order of evaluation and the exception.
let order = '';
const a = { valueOf() { order += 'a'; return 9; } };
console.log('order', bits(a / 2), order);
try {
  const bad = { valueOf() { throw new RangeError('no'); } };
  console.log(bad / 2);
} catch (e) {
  console.log('threw', e.constructor.name, e.message);
}
