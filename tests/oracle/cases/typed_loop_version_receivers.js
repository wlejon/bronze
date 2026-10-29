// Typed-loop versioning (src/lower/typed_loop_version.h): which views and
// which indexes the typed copy of a loop must still answer exactly as the
// generic one would. A loop with no call in it also reads `length` once on
// entry, so an own `length`, a patched prototype getter, and a length-tracking
// view have to send it back to the original loop.

function sumTo(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i];
  return s;
}

function sumN(a, from, n) {
  let s = 0;
  for (let i = from; i < n; i++) s += a[i];
  return s;
}

function scale(a, k) {
  for (let i = 0; i < a.length; i++) a[i] = a[i] * k;
}

function scaleFrom(a, from, k) {
  for (let i = from; i < a.length; i++) a[i] = a[i] * k;
}

// `scale` over a fresh 1..4 view, as the list it leaves.
function scaled(Kind, k) {
  const v = Kind.of(1, 2, 3, 4);
  scale(v, k);
  return Array.from(v).join(',');
}

function copy(dst, src, n) {
  for (let i = 0; i < n; i++) dst[i] = src[i];
}

// Half steps: every other index is not an array index, so it reads
// `undefined` and its store goes nowhere.
function halves(a) {
  let s = 0;
  for (let i = 0; i < 4; i += 0.5) {
    const v = a[i] - 0;
    s += v === v ? v : 100;
    a[i] = v + 1;
  }
  return s;
}

// The variable is rebound mid-loop: later reads see the new view.
function swapMid(a, b, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s += a[i];
    if (i === 1) a = b;
  }
  return s;
}

// Reads whose value is not coerced to a number keep `undefined`.
function holes(a, n) {
  const out = [];
  for (let i = -1; i < n; i++) out.push(a[i] === undefined ? 'u' : a[i]);
  return out.join(',');
}

function fill(a, v) {
  for (let i = 0; i < a.length; i++) a[i] = v;
}

const list = (v) => Array.from(v).join(',');

// Warm every loop on views it versions before the odd receivers arrive.
let warm = 0;
for (let r = 0; r < 300; r++) {
  const f = new Float64Array([1, 2, 3, 4]);
  const g = new Float32Array([1, 2, 3, 4]);
  warm += sumTo(f) + sumTo(g) + sumN(f, 0, 4) + sumN(g, 0, 4);
  scale(f, 2);
  scaleFrom(g, 0, 1);
  copy(g, f, 4);
  warm += g[3] + halves(new Float64Array(4)) + swapMid(f, g, 4);
}
console.log('warm', warm);

const f64 = Float64Array.of(1, 2, 3, 4, 5, 6, 7, 8);
const f32 = Float32Array.of(1, 2, 3, 4, 5, 6, 7, 8);
console.log('sum', sumTo(f64), sumTo(f32), sumTo([1, 2, 3]), sumTo(Int16Array.of(4, 5)));
console.log('past end', sumN(f64, 0, 9), sumN(f32, 6, 8), sumN(f64, -1, 2));
const neg = Float64Array.of(1, 2, 3);
scaleFrom(neg, -2, 3);
console.log('from', list(neg));
console.log('halves', halves(Float64Array.of(1, 2, 3, 4)), halves(Float32Array.of(1, 2, 3, 4)));
const h = Float64Array.of(1, 2, 3, 4);
halves(h);
console.log('halves stored', list(h));
console.log('swap', swapMid(Float64Array.of(1, 2, 3, 4), Float32Array.of(10, 20, 30, 40), 4),
  swapMid(Float64Array.of(1, 2, 3, 4), [10, 20, 30, 40], 4),
  swapMid(Float64Array.of(1, 2, 3, 4), Float64Array.of(10, 20), 4));
console.log('holes', holes(Float64Array.of(1.5, 2.5), 3), holes(Float32Array.of(0.5), 2));

// A subarray starts at an offset into its buffer and ends before its end.
const big = Float64Array.of(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
const sub = big.subarray(2, 6);
scale(sub, 2);
console.log('subarray', sumTo(sub), sub.length, list(big));

// Float32 stores round; the read sees the rounded value.
const tenth = Float32Array.of(0.1);
scale(tenth, 3);
console.log('round', tenth[0], sumTo(Float32Array.of(0.1, 0.2)));

// Into a narrower kind, past the source's end, and into a kind not versioned.
const src = Float64Array.of(1.5, 2.25, 1e40, -3);
const dst = new Float32Array(6);
copy(dst, src, 6);
const ints = new Int32Array(3);
copy(ints, Float64Array.of(1.9, -2.9, 7), 3);
const plain = [];
copy(plain, src, 2);
console.log('copy', list(dst), list(ints), plain.join(','));

// A stored object converts once per store.
let calls = 0;
const filled = new Float64Array(3);
fill(filled, { valueOf() { calls++; return 7; } });
console.log('fill', list(filled), calls);

// A length-tracking view's length follows its buffer.
const rab = new ArrayBuffer(32, { maxByteLength: 64 });
const tracking = new Float64Array(rab);
tracking.set([1, 2, 3, 4]);
console.log('tracking', sumTo(tracking));
rab.resize(16);
scale(tracking, 2);
console.log('tracking shrunk', sumTo(tracking), list(tracking));
rab.resize(48);
tracking[5] = 10;
scale(tracking, 2);
console.log('tracking grown', sumTo(tracking), list(tracking));

// An own `length` shadows the prototype's.
const own = Float64Array.of(1, 2, 3, 4, 5);
Object.defineProperty(own, 'length', { value: 2 });
scale(own, 10);
console.log('own length', sumTo(own), sumTo(Float64Array.of(1, 2, 3, 4, 5)), list(own));

// A getter on one kind's prototype, then on %TypedArray%.prototype.
Object.defineProperty(Float32Array.prototype, 'length', { get() { return 3; }, configurable: true });
console.log('kind getter', sumTo(f32), sumTo(f64), scaled(Float32Array, 2), scaled(Float64Array, 2));
delete Float32Array.prototype.length;
console.log('kind getter gone', sumTo(f32), scaled(Float32Array, 2));
const base = Object.getPrototypeOf(Float64Array.prototype);
const original = Object.getOwnPropertyDescriptor(base, 'length');
Object.defineProperty(base, 'length', { get() { return 2; }, configurable: true });
console.log('base getter', sumTo(f32), sumTo(f64), scaled(Float32Array, 2), scaled(Float64Array, 2));
Object.defineProperty(base, 'length', original);
console.log('base restored', sumTo(f32), sumTo(f64), scaled(Float32Array, 2), scaled(Float64Array, 2));
