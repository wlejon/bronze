// `===` and `!==` between two values whose BITS are equal and at least one of
// which is not a Number: generated code answers those by a branch before any
// tag is read (il_lowering_ops.cpp lower_dynamic_strict_eq). Equal bits are
// one String, object, symbol, BigInt or primitive constant, so the answer is
// true; the pair whose equal bits are NOT equal values, NaN and NaN, is a
// Number pair and must still say false. And every pair with DIFFERENT bits
// must still reach its own answer: equal strings built apart, equal BigInts
// built apart, different strings of one length, different kinds.

// Generic: called with every kind, so its `===` is the dynamic one.
function eq(a, b) { return a === b; }
function ne(a, b) { return a !== b; }

const built = (s) => s.split('').reverse().reverse().join('');
const obj = { k: 1 };
const arr = [1, 2];
const sym = Symbol('s');
const fn = () => 1;
const big = 12345678901234567890n;
const pairs = [
  ['same string', 'XYZ', 'XYZ'],
  ['string built apart', 'XYZ', built('XYZ')],
  ['string same length', 'XYZ', 'XZY'],
  ['empty strings', '', ''],
  ['long string', 'a'.repeat(100), 'a'.repeat(100)],
  ['same object', obj, obj],
  ['equal objects', obj, { k: 1 }],
  ['same array', arr, arr],
  ['same symbol', sym, sym],
  ['other symbol', sym, Symbol('s')],
  ['same function', fn, fn],
  ['same bigint', big, big],
  ['bigint built apart', big, BigInt('12345678901234567890')],
  ['small bigints', 5n, 5n],
  ['bigint vs number', 5n, 5],
  ['true true', true, true],
  ['true false', true, false],
  ['null null', null, null],
  ['undefined undefined', undefined, undefined],
  ['null undefined', null, undefined],
  ['NaN NaN', NaN, NaN],
  ['0 -0', 0, -0],
  ['1 "1"', 1, '1'],
  ['string vs object', 'k', obj],
];
for (const [name, a, b] of pairs) {
  console.log(name, eq(a, b), ne(a, b), eq(b, a));
}

// Hot, so every tier compiles the generic compare; the answers cannot change
// with the tier.
let counts = [0, 0, 0, 0];
for (let r = 0; r < 4000; r++) {
  const [, a, b] = pairs[r % pairs.length];
  if (eq(a, b)) counts[0]++;
  if (eq(a, a)) counts[1]++;
  if (ne(b, b)) counts[2]++;
  if (eq(b, a)) counts[3]++;
}
console.log('hot', counts.join(' '));

// A switch on a string field, the shape Quaternion.setFromEuler has: the
// interned case constant against a field that holds the same constant, one
// built at run time, and one that matches no case.
class Rot {
  constructor(order) { this.order = order; }
  pick() {
    switch (this.order) {
      case 'XYZ': return 1;
      case 'YXZ': return 2;
      case 'ZXY': return 3;
      default: return 0;
    }
  }
}
const orders = ['XYZ', 'YXZ', built('ZXY'), built('XYZ'), 'XZY', 'xyz', '', 'XYZW'];
let picked = '';
for (let r = 0; r < 3000; r++) {
  const o = new Rot(orders[r % orders.length]);
  if (r < orders.length) picked += o.pick();
  else if (o.pick() !== new Rot(o.order).pick()) picked += '!';
}
console.log('switch', picked);

// An object compared with the field that holds it, and with a copy.
const holder = { ref: obj, other: { k: 1 } };
let hits = 0;
for (let i = 0; i < 3000; i++) {
  if (holder.ref === obj) hits++;
  if (holder.other === obj) hits += 1000;
  if (holder.ref !== obj) hits += 1000000;
}
console.log('fields', hits);

// NaN from a field, from a typed array, and boxed through an array: never
// equal to itself, with whatever bits it is held in.
const holderNaN = { v: 0 / 0 };
const fa = new Float64Array([NaN]);
const boxes = [NaN, holderNaN.v, fa[0]];
let nanEq = 0;
for (let i = 0; i < 3000; i++) {
  const x = boxes[i % 3];
  if (x === x) nanEq++;
  if (eq(x, boxes[(i + 1) % 3])) nanEq++;
}
console.log('nan', nanEq);
