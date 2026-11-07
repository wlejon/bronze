// Doubles stored into arrays, object slots and typed arrays by kernels whose
// stores canonicalize NaN in the FP domain: every NaN (any payload, either
// sign, and the negative default NaN x86 arithmetic makes) reads back as NaN,
// -0 keeps its sign, and ordinary values are untouched.
function nanBits(hi, lo) {
  const dv = new DataView(new ArrayBuffer(8));
  dv.setUint32(0, hi); dv.setUint32(4, lo);
  return dv.getFloat64(0);
}
const payloads = [nanBits(0x7ff80000, 0), nanBits(0xfff80000, 0), nanBits(0x7ff00000, 1),
                  nanBits(0xfff12345, 0x6789abcd), nanBits(0x7fffffff, 0xffffffff), nanBits(0xfff7ffff, 0xffffffff)];

class M {
  constructor() { this.elements = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }
  mul(a, b) {
    const ae = a.elements, be = b.elements, te = this.elements;
    const a11 = ae[0], a12 = ae[4], a21 = ae[1], a22 = ae[5];
    const b11 = be[0], b12 = be[4], b21 = be[1], b22 = be[5];
    te[0] = a11 * b11 + a12 * b21; te[4] = a11 * b12 + a12 * b22;
    te[1] = a21 * b11 + a22 * b21; te[5] = a21 * b12 + a22 * b22;
    te[2] = a11 - b11; te[3] = a12 * b12; te[6] = -a21; te[7] = a22 / b22;
    return this;
  }
}
class P { constructor() { this.x = 0; this.y = 0; this.z = 0; } }
function setP(p, a, b) { p.x = a * b; p.y = a + b; p.z = a - b; return p; }
function fill(f, a, b) { f[0] = a * b; f[1] = a + b; f[2] = b - a; f[3] = a / b; return f; }

function describe(v) {
  if (Number.isNaN(v)) return 'NaN';
  if (Object.is(v, -0)) return '-0';
  return String(v);
}
function show(label, arr, n) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(describe(arr[i]));
  console.log(label, out.join(' '));
}

const inputs = [[2, 3], [-0, 5], [0, -0], [Infinity, 0], [Infinity, -Infinity], [-Infinity, -Infinity], [1e308, 10]]
  .concat(payloads.map(p => [p, 2])).concat(payloads.map(p => [3, p]));
const m = new M(), a = new M(), b = new M();
let k = 0;
for (let round = 0; round < 3000; round++) {
  const [x, y] = inputs[round % inputs.length];
  a.elements[0] = x; a.elements[5] = y; a.elements[1] = y; a.elements[4] = x;
  b.elements[0] = y; b.elements[5] = x; b.elements[1] = x; b.elements[4] = y;
  m.mul(a, b);
  if (round >= 3000 - inputs.length) show('m' + k++, m.elements, 8);
}
const p = new P();
for (const [x, y] of inputs) { setP(p, x, y); console.log('p', describe(p.x), describe(p.y), describe(p.z)); }
const f64 = new Float64Array(4), f32 = new Float32Array(4), arr = [0, 0, 0, 0];
for (const [x, y] of inputs) {
  fill(f64, x, y); fill(f32, x, y); fill(arr, x, y);
  show('f', f64, 4); show('g', f32, 4); show('a', arr, 4);
}
// A NaN read back from storage goes on to arithmetic and comparisons as a NaN.
let nanCount = 0;
for (let i = 0; i < 8; i++) {
  const v = m.elements[i];
  if (v !== v) nanCount++;
  if (Object.is(v, NaN) !== Number.isNaN(v)) console.log('mismatch', i);
}
console.log('nans', nanCount, [m.elements[0], p.x].map(describe).join(','), JSON.stringify(arr.map(describe)));
const s = new Set([nanBits(0xfff12345, 1) * 1, NaN, payloads[3] + 0]);
console.log('set', s.size, s.has(NaN), [NaN].includes(payloads[5] - 1), [payloads[4] * 2].indexOf(NaN));
