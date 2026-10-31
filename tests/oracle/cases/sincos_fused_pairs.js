// A sine and a cosine of one value, which brass's sincos_fuse answers with one
// call (one argument reduction): every result printed as its exact bits, so a
// fused pair must be bit-identical to the two separate calls in every range of
// the reduction, and the program shapes around the pair (branches, loops,
// aliases, valueOf, generators, exceptions) must keep their meaning.

const f64 = new Float64Array(1);
const u32 = new Uint32Array(f64.buffer);
function bits(x) {
  // A NaN's bits in a Float64Array are the implementation's choice (ECMA-262
  // NumericToRawBytes): only that it is a NaN is printed.
  if (x !== x) return 'NaN';
  f64[0] = x;
  return u32[1].toString(16).padStart(8, '0') + u32[0].toString(16).padStart(8, '0');
}

// Inputs over the reduction's ranges: tiny, by pi/4, medium, at multiples of
// pi/2 (fdlibm's cancellation steps and its x-near-pi/2 split), past 2^19 pi/2
// (Payne-Hanek), huge, the specials.
const inputs = [
  0, -0, 5e-324, -5e-324, 1e-310, 2.2250738585072014e-308, 1e-20, -1e-20, 7.450580596923828e-9,
  1.4901161193847656e-8, 3.7252902984e-9, 0.1, -0.1, 0.5, 0.7853981633974483, 0.7853981633974484,
  -0.7853981633974483, 0.785398163397448, 1, -1, 1.5707963267948966, -1.5707963267948966,
  1.5707963267948968, 2, 2.356194490192345, 2.356194490192346, 3, 3.141592653589793,
  -3.141592653589793, 4.71238898038469, 6.283185307179586, 10, -10, 100, 1000, 1e4, 1e5,
  411774.81771690346, 823549.6354338069, 823549.6354338070, 1e6, -1e6, 2e6, 1e7, 1e9, 1e15,
  1e22, -1e22, 1e100, 1.7976931348623157e308, -1.7976931348623157e308, Infinity, -Infinity, NaN,
  // near multiples of pi/2 where cancellation takes the second and third steps
  52.40928954087346, 1.5707963267948966 * 33, 1.5707963267948966 * 1000, 1.5707963267948966 * 6381956,
  1.5707963267948966 * 123456, 13176794.2, 6.7e6, 2.5e7
];

// Each takes its operand through `+x` first: one f64 value, so the sine's and
// the cosine's operand are the same SSA value and the pair fuses. (Two
// Math.sin(x) of a dynamic x each run their own ToNumber, which for an object
// is a valueOf call each; those are kept apart, see onObject below.)
function pair(x) {
  const v = +x;
  const c = Math.cos(v);
  const s = Math.sin(v);
  return bits(s) + ' ' + bits(c);
}
function pairReversed(x) {
  const v = +x;
  const s = Math.sin(v);
  const c = Math.cos(v);
  return bits(s) + ' ' + bits(c);
}
function halfAngles(x) {
  // setFromEuler's shape: the operand recomputed for each call
  const cos = Math.cos;
  const sin = Math.sin;
  const v = +x;
  const c1 = cos(v / 2);
  const s1 = sin(v / 2);
  return bits(s1) + ' ' + bits(c1);
}
for (const x of inputs) {
  const a = pair(x);
  const b = pairReversed(x);
  const c = halfAngles(x * 2);
  console.log(bits(x), a, a === b ? 'same' : 'DIFF ' + b, c);
}

// setFromEuler itself, three angles, every sum and product printed.
function quat(x0, y0, z0) {
  const x = +x0, y = +y0, z = +z0;
  const c1 = Math.cos(x / 2), c2 = Math.cos(y / 2), c3 = Math.cos(z / 2);
  const s1 = Math.sin(x / 2), s2 = Math.sin(y / 2), s3 = Math.sin(z / 2);
  return [
    s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3,
    c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3,
  ].map(bits).join(' ');
}
let seed = 12345;
function rnd() { seed = (seed * 16807) % 2147483647; return (seed - 1) / 2147483646; }
for (let i = 0; i < 40; i++) {
  console.log('quat', quat(rnd() * 20 - 10, rnd() * 1e3, (rnd() - 0.5) * 1e7));
}

// Only one of the two on each path: neither call dominates the other.
function branchy(x0, which) {
  const x = +x0;
  if (which) return 'sin ' + bits(Math.sin(x));
  return 'cos ' + bits(Math.cos(x));
}
for (const x of [0.3, 2, 1e6, -0]) console.log(branchy(x, true), branchy(x, false));

// One before a branch, the other inside it; and the cosine in a loop over a sine
// taken once.
function partial(x0, n) {
  const x = +x0;
  const s = Math.sin(x);
  let acc = 0;
  if (n > 2) acc += Math.cos(x);
  for (let i = 0; i < n; i++) acc += Math.cos(x) * i;
  return bits(s) + ' ' + bits(acc);
}
for (const n of [0, 1, 3, 7]) console.log('partial', n, partial(1.25 * n + 0.5, n));

// Different operands that look alike must not pair.
function nearly(x0) {
  const x = +x0;
  return bits(Math.sin(x)) + ' ' + bits(Math.cos(x + 0)) + ' ' + bits(Math.cos(x * 1)) + ' ' + bits(Math.cos(-x));
}
console.log('nearly', nearly(-0), nearly(0.75), nearly(NaN));

// A dynamic operand (each call converts it on its own).
function dynPair(x) {
  return bits(Math.sin(x)) + ' ' + bits(Math.cos(x));
}
for (const x of [0.5, 3e6, -0, '2', null]) console.log('dyn', dynPair(x));

// An object operand: ToNumber runs once per call, and each call may see a
// different number.
let calls = 0;
const moving = { valueOf() { calls++; return calls * 0.5; } };
function onObject(o) {
  const s = Math.sin(o);
  const c = Math.cos(o);
  return bits(s) + ' ' + bits(c);
}
console.log('valueOf', onObject(moving), calls);
console.log('valueOf', onObject(moving), calls);
console.log('strings', onObject('1.5'), onObject(''), onObject('x'), onObject(null), onObject(undefined), onObject(true));

// The pair across a yield, and across a throw caught in the same function.
function* gen(x0) {
  const x = +x0;
  const s = Math.sin(x);
  yield bits(s);
  yield bits(Math.cos(x));
}
console.log('gen', [...gen(4.5)].join(' '));
function guarded(x0) {
  const x = +x0;
  let s = 0;
  try {
    s = Math.sin(x);
    if (x > 1) throw new Error('big');
  } catch (e) {
    return 'caught ' + bits(s) + ' ' + bits(Math.cos(x));
  }
  return 'plain ' + bits(s) + ' ' + bits(Math.cos(x));
}
console.log(guarded(0.5), guarded(5));

// Many pairs in one function, sharing operands among them.
function many(a0, b0) {
  const a = +a0, b = +b0;
  const r = [];
  for (const v of [a, b, a + b, a - b, a * b, a / b]) r.push(Math.sin(v) + Math.cos(v));
  r.push(Math.sin(a) * Math.cos(b) - Math.cos(a) * Math.sin(b));
  return r.map(bits).join(' ');
}
console.log('many', many(1.1, 0.4));
console.log('many', many(1e6, -3));
