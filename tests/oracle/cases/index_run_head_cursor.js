// Constant-index read and store runs over arrays whose elements start past
// the block's first slot (a `shift()` moves the head): every load and store
// of a run addresses `block + head*8 + disp`, the head read from the array
// each call. The arrays' heads change between calls, the target aliases a
// source, and a frozen or shrunk target takes the slow path, so a run that
// addressed with a stale or missing head reads or writes the wrong slots.
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
  copy(m) {
    const te = this.elements, me = m.elements;
    te[0] = me[0]; te[1] = me[1]; te[2] = me[2]; te[3] = me[3];
    te[4] = me[4]; te[5] = me[5]; te[6] = me[6]; te[7] = me[7];
    te[8] = me[8]; te[9] = me[9]; te[10] = me[10]; te[11] = me[11];
    te[12] = me[12]; te[13] = me[13]; te[14] = me[14]; te[15] = me[15];
    return this;
  }
}
const fmt = v => (typeof v === 'number' ? (Object.is(v, -0) ? '-0' : String(v)) : typeof v + ':' + String(v));
function show(label, m) {
  const e = m.elements;
  console.log(label, e.length, Array.from({ length: e.length }, (_, i) => fmt(e[i])).join(' '));
}
function seq(base) { return Array.from({ length: 16 }, (_, i) => base + i * 0.5); }
// `lead` extra elements in front, shifted off: the 16 left start at head `lead`.
function headed(base, lead) {
  const e = seq(base);
  for (let i = 0; i < lead; i++) e.unshift(-1000 - i);
  for (let i = 0; i < lead; i++) e.shift();
  return e;
}

const a = new M(), b = new M(), m = new M();
let sum = 0;
for (let k = 0; k < 3000; k++) {
  a.elements = headed(k & 7, k % 3);
  b.elements = headed(2 - (k & 3), (k >> 1) % 4);
  m.elements = headed(0, k % 5);
  m.mul(a, b);
  for (let i = 0; i < 16; i++) sum += m.elements[i];
}
console.log('warm', sum);

for (const [la, lb, lt] of [[0, 0, 0], [1, 0, 0], [0, 3, 0], [0, 0, 2], [5, 6, 7], [16, 1, 9]]) {
  a.elements = headed(1, la); b.elements = headed(-2, lb); m.elements = headed(0, lt);
  m.mul(a, b); show(`mul.${la}.${lb}.${lt}`, m);
  m.copy(a); show(`copy.${la}.${lt}`, m);
}

// The target is a source: the reads see the elements before any store.
a.elements = headed(1, 4); b.elements = headed(3, 2);
a.mul(a, b); show('alias.a', a);
b.elements = headed(3, 2);
b.mul(a, b); show('alias.b', b);
a.elements = headed(0.25, 3);
a.mul(a, a); show('alias.both', a);
a.elements = headed(0.25, 3);
a.copy(a); show('alias.copy', a);

// A shift between calls moves the head of an array already read once.
const moving = headed(1, 2);
a.elements = moving; b.elements = headed(2, 1); m.elements = headed(0, 0);
m.mul(a, b); show('moving.0', m);
moving.unshift(42); m.mul(a, b); show('moving.unshift', m);
moving.shift(); moving.shift(); m.mul(a, b); show('moving.shift2', m);
m.elements = moving; m.mul(m, b); show('moving.target', m);

// Targets the store run must refuse: frozen, too short, grown, sparse.
const frozen = Object.freeze(headed(0, 2));
m.elements = frozen;
try { m.mul(a, b); console.log('frozen: stored'); } catch (e) { console.log('frozen:', e.constructor.name); }
show('frozen', m);
const short = headed(0, 3); short.length = 9;
m.elements = short; m.mul(a, b); show('short', m);
m.copy(b); show('short.copy', m);
const grown = headed(0, 1); grown.push(7, 8);
m.elements = grown; m.mul(a, b); show('grown', m);
const sparse = []; sparse[15] = 0;
m.elements = sparse; m.mul(a, b); show('sparse', m);
m.elements = headed(0, 6); m.mul(a, b); show('back', m);
