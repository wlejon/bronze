// The arms of an inline path (il_speculation.h, "ARMS"): a truthiness test
// that sees one kind of value for a long run and then every other kind, a
// store that goes into one element kind of view and then into Arrays, other
// views and non-objects, `+ - * /` on Numbers that then meet strings, BigInts
// and objects with valueOf, and `===` of Numbers, of objects and of one
// interned string against itself that then compares every kind with every
// other. Built on its own harvest every arm here
// ran, so each test stays; the answers must be node's in that build and in
// the plain one, whose arms are marked as they run.

function truth(o) {
  return o.v ? 1 : 0;
}
function truthNum(o) {
  let n = 0;
  if (o.v) n += 1;
  if (!o.w) n += 2;
  return n;
}
function fill(o, n, x) {
  const a = o.buf;
  for (let i = 0; i < n; i++) a[i] = x + i;
  return a[0] + a[n - 1];
}
function arith(o, k) {
  let s = o.a + k;
  s = s - o.b;
  s = s * o.a;
  return s / k;
}

function eqNum(o, p) {
  return o.a === p.a ? 1 : 0;
}
function eqRef(o, p) {
  return o.r === p.r ? 1 : 0;
}
function eqMode(o, p) {
  return o.mode !== p.mode ? 1 : 0;
}

let total = 0;
const flag = { v: true }, obj = { v: {} }, num = { v: 3, w: 0 }, str = { v: 'x', w: '' };
const f32 = { buf: new Float32Array(8) }, f64 = { buf: new Float64Array(8) };
const u8c = { buf: new Uint8ClampedArray(8) }, i16 = { buf: new Int16Array(8) }, arr = { buf: [0, 0, 0, 0, 0, 0, 0, 0] };
const nums = { a: 1.5, b: 2 };
for (let i = 0; i < 30000; i++) {
  total += truth(flag) + truth(obj) * 2 + truthNum(num) + truthNum(str);
  total += fill(f32, 8, i) + fill(f64, 8, 0.5) + fill(u8c, 8, 250) + fill(i16, 8, 32760) + fill(arr, 8, 1);
  total += arith(nums, 3);
  total += eqNum(nums, { a: i & 1 ? 1.5 : 2.5 }) + eqRef({ r: flag }, { r: i & 1 ? flag : obj });
  total += eqMode({ mode: 'normal' }, { mode: 'normal' });
}
console.log(total);

const pairs = [1.5, NaN, 0, -0, '1.5', 'ab', 'a' + 'b', 3n, 3n + 0n, null, undefined, true, flag, obj, Symbol.iterator];
const eqs = [];
for (const x of pairs) {
  for (const y of pairs) eqs.push(eqNum({ a: x }, { a: y }) + eqRef({ r: x }, { r: y }) + eqMode({ mode: x }, { mode: y }));
}
console.log(eqs.join(''));

const odd = [false, null, undefined, 0, -0, NaN, 1e300, '', 'a', 0n, 7n, {}, [], Symbol.iterator, () => 0, new Number(0)];
const out = [];
for (const v of odd) {
  out.push(truth({ v }), truthNum({ v, w: v }));
}
console.log(out.join(''));

const stores = [new Float32Array(4), new Float64Array(4), new Uint8ClampedArray(4), new Int8Array(4), new Uint32Array(4),
  [1, 2], [], { length: 4 }, new BigInt64Array(4), 'text', 5];
for (const buf of stores) {
  for (const x of [1.25, -1, 300, NaN, 'q', 2n]) {
    let r;
    try { r = String(fill({ buf }, 3, x)); } catch (e) { r = 'throws ' + e.constructor.name; }
    out.push(r);
  }
}
console.log(out.join(' '));

const operands = [1, 'two', 3n, { valueOf() { return 4; } }, null, undefined, [5], true];
const res = [];
for (const a of operands) {
  for (const k of [2, '3', 4n]) {
    try { res.push(String(arith({ a, b: a }, k))); } catch (e) { res.push('throws ' + e.constructor.name); }
  }
}
console.log(res.join(' '));

for (let i = 0; i < 3000; i++) {
  total += truth(flag) + truth(obj) * 2 + truthNum(num) + truthNum(str) + fill(f32, 8, i) + arith(nums, 3);
  total += eqNum(nums, nums) + eqRef({ r: obj }, { r: obj });
  total += eqMode({ mode: 'normal' }, { mode: i % 7 ? 'normal' : 'add' + i });
  if (i % 50 === 0) total += truth({ v: 'late' }) + fill(arr, 8, i) + arith({ a: '1', b: 1 }, 2);
}
console.log(total);
