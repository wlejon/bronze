// Constant-index read runs on two receivers behind `is.number_array` claims:
// the claims stand in for the runs' dense-array guards. Receivers that change
// kind between calls (a string element, a hole, an Int32 from a bitwise op,
// a named property, shrinking below the indices read, a typed array, an
// object with index keys, the same array on both sides, a getter on
// Array.prototype seen through a hole) take the slow path and read what the
// program reads.
class M {
  constructor() { this.elements = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }
  mul(a, b) {
    const ae = a.elements, be = b.elements, te = this.elements;
    const a11 = ae[0], a12 = ae[4], a13 = ae[8], a14 = ae[12];
    const a21 = ae[1], a22 = ae[5], a23 = ae[9], a24 = ae[13];
    const a31 = ae[2], a32 = ae[6], a33 = ae[10], a34 = ae[14];
    const a41 = ae[3], a42 = ae[7], a43 = ae[11], a44 = ae[15];
    const b11 = be[0], b12 = be[4], b13 = be[8], b14 = be[12];
    const b21 = be[1], b22 = be[5], b23 = be[9], b24 = be[13];
    const b31 = be[2], b32 = be[6], b33 = be[10], b34 = be[14];
    const b41 = be[3], b42 = be[7], b43 = be[11], b44 = be[15];
    te[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
    te[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
    te[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
    te[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;
    te[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
    te[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
    te[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
    te[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;
    te[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
    te[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
    te[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
    te[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;
    te[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
    te[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
    te[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
    te[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
    return this;
  }
}
const fmt = v => (typeof v === 'number' ? (Object.is(v, -0) ? '-0' : String(v)) : typeof v + ':' + String(v));
function show(label, m) { console.log(label, Array.from({ length: 16 }, (_, i) => fmt(m.elements[i])).join(' ')); }
function seq(base) { return Array.from({ length: 16 }, (_, i) => base + i * 0.5); }

const m = new M(), a = new M(), b = new M();
a.elements = seq(1); b.elements = seq(-3);
let acc = 0;
for (let i = 0; i < 20000; i++) { m.mul(a, b); acc += m.elements[i & 15]; }
console.log('warm', acc);
show('base', m);

const variants = [
  ['string', e => { e[5] = '2'; }],
  ['hole', e => { delete e[9]; }],
  ['int32', e => { e[3] = e[3] | 0; e[7] = 7 >> 1; }],
  ['named', e => { e.extra = 1; }],
  ['short', e => { e.length = 10; }],
  ['grown', e => { e.push(99, 100); }],
  ['negzero', e => { e[0] = -0; e[4] = -0; }],
  ['nan', e => { e[1] = NaN; }],
  ['bool', e => { e[2] = true; }],
  ['undef', e => { e[6] = undefined; }],
  ['object', e => { e[11] = { valueOf() { return 4; } }; }],
];
for (const [name, mutate] of variants) {
  for (const side of ['a', 'b']) {
    a.elements = seq(1); b.elements = seq(-3);
    mutate(side === 'a' ? a.elements : b.elements);
    try { m.mul(a, b); show(name + '.' + side, m); } catch (e) { console.log(name + '.' + side, 'threw', e.constructor.name); }
    // and back to a number array: the claim scans again
    a.elements = seq(2); b.elements = seq(-1);
    m.mul(a, b); show(name + '.' + side + '.back', m);
  }
}
// Not arrays at all.
a.elements = new Float64Array(seq(1)); b.elements = new Float32Array(seq(-3)); m.mul(a, b); show('typed', m);
a.elements = Object.assign({}, seq(1)); b.elements = seq(-3); m.mul(a, b); show('object', m);
a.elements = 'abcdefghijklmnop'; b.elements = seq(-3); m.mul(a, b); show('string', m);
// The same array everywhere: the stores land in the array the reads came from.
const shared = new M(); shared.elements = seq(0.25);
shared.mul(shared, shared); show('alias', shared);
a.elements = seq(1); m.mul(a, m); show('alias-te', m);
// A hole seen through a custom prototype's getter.
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, 13, { get() { return 1000; } });
a.elements = seq(1); delete a.elements[13]; Object.setPrototypeOf(a.elements, proto); b.elements = seq(-3);
m.mul(a, b); show('proto-getter', m);
a.elements = seq(1); b.elements = seq(-3); m.mul(a, b); show('after-proto', m);
