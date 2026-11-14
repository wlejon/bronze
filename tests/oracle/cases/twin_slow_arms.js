// Every kind of slow arm a harvest can turn into an exit to the resume twin
// (il_speculation.h, "SLOW ARMS"), reached after a long run on the fast
// paths: an element read out of bounds and of a hole, a receiver that is no
// Array, a getter that appears on the prototype chain, a value that needs a
// conversion, a strict compare of two strings with equal lengths, a
// truthiness test of a BigInt, and constant-index runs on other receivers.
// Each function reads a property first, so it has a twin; built on a harvest
// that saw none of these arms run, each one leaves for it.

function sum(o, n) {
  const a = o.items;
  let s = 0;
  for (let i = 0; i < n; i++) {
    const v = a[i];
    s += v === undefined ? 1000 : v;
  }
  return s;
}

function scale(o, k) {
  const v = o.value;
  return v * k;
}

function same(o, t) {
  return o.name === t;
}

function truthy(o) {
  return o.flag ? 1 : 0;
}

function copy4(o, src) {
  const d = o.dst;
  d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
  return d[0] + d[1] + d[2] + d[3];
}

function first3(o) {
  const a = o.items;
  return a[0] + a[1] + a[2];
}

const arr = [1, 2, 3, 4, 5, 6, 7, 8];
const box = { items: arr };
let s = 0;
for (let r = 0; r < 3000; r++) s += sum(box, 8);
console.log(s);
// Out of bounds, then a hole.
console.log(sum(box, 10));
const holey = [1, 2, , 4];
console.log(sum({ items: holey }, 4));
// No Array: a typed array, a string, an object with index keys.
console.log(sum({ items: new Float64Array([1.5, 2.5]) }, 3));
console.log(sum({ items: 'abc' }, 3));
console.log(sum({ items: { 0: 7, 1: 8, length: 2 } }, 2));

let t = 0;
const num = { value: 3 };
for (let r = 0; r < 3000; r++) t += scale(num, 2);
console.log(t);
console.log(scale({ value: '4' }, 2), scale({ value: { valueOf() { return 5; } } }, 2), scale({ value: undefined }, 2));

let u = 0;
const named = { name: 'alpha' };
for (let r = 0; r < 3000; r++) u += same(named, 'alpha') ? 1 : 0;
console.log(u);
console.log(same({ name: 'alphb' }, 'alpha'), same({ name: 'alp' + 'ha' }, 'alpha'), same({ name: 1n }, 1n));

let w = 0;
const flagged = { flag: true };
for (let r = 0; r < 3000; r++) w += truthy(flagged);
console.log(w);
console.log(truthy({ flag: 0n }), truthy({ flag: 2n }), truthy({ flag: '' }), truthy({ flag: 'x' }));

let x = 0;
const dst = { dst: [0, 0, 0, 0] };
const src = [1, 2, 3, 4];
for (let r = 0; r < 3000; r++) x += copy4(dst, src);
console.log(x);
console.log(copy4({ dst: new Float32Array(4) }, src), copy4(dst, [1, 2]), copy4(dst, 'wxyz'));

let y = 0;
for (let r = 0; r < 3000; r++) y += first3(box);
console.log(y);
console.log(first3({ items: [1, 2] }), first3({ items: new Int8Array([5, 6, 7]) }), first3({ items: 'pqr' }));

// A getter on the prototype chain (the intrinsics are frozen, so an array's
// own prototype carries it): a hole and a read past the end now ask it.
const getters = Object.create(Array.prototype, {
  2: { get() { return 7; }, configurable: true },
  9: { get() { return 90; }, configurable: true },
});
const viaGetter = Object.setPrototypeOf([1, 2, 3, 4, 5, 6, 7, 8], getters);
console.log(sum({ items: viaGetter }, 10), sum({ items: Object.setPrototypeOf([1, 2, , 4], getters) }, 4));
delete getters[9];
console.log(sum({ items: viaGetter }, 10));
