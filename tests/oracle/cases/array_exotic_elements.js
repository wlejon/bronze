// Exotic array elements (runtime/rt_array_exotic.cpp): an accessor element
// and data elements with attributes of their own (non-writable,
// non-enumerable, non-configurable) defined by Object.defineProperty on
// existing elements and past the length, read, written and enumerated
// through every path: index reads and writes, JSON.stringify, Array.isArray,
// indexOf / includes / forEach / map / sort / splice / push / pop / concat /
// Array.from / at / findIndex, spread, for-of, for-in, Object.keys / entries
// / values / getOwnPropertyNames / getOwnPropertyDescriptor, hasOwnProperty,
// `in`, delete, `length` truncation stopped by a non-configurable element,
// and freeze / seal / preventExtensions with their predicates. The hot loop
// is trained on plain arrays (run.py's switch mode runs the real arm on a
// harvest of the training arm), so its compiled element reads, stores,
// pushes and pops meet an exotic array they never saw.
const TRAINING = Math.random() < 0.5;

function hot(arr, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const v = arr[i % arr.length];
    s += typeof v === 'number' ? v : v.length;
    arr[i % arr.length] = typeof v === 'number' ? v + 1 : v;
  }
  arr.push(s % 7);
  arr.pop();
  return s;
}

function train() {
  let t = 0;
  for (let r = 0; r < 300; r++) t += hot([1, 2, 3, 4, 5, 6, 7, 8], 64);
  return t;
}

function real() {
  const out = [];
  const a = [1, 2, 3, 4, 5];
  let calls = 0;
  Object.defineProperty(a, 1, { get() { calls++; return 'g' + this.length; }, set(v) { out.push('set ' + v); }, configurable: true, enumerable: true });
  out.push(a[1], a.length, calls, JSON.stringify(a), Array.isArray(a));
  a[1] = 'x';
  out.push(a.indexOf('g5'), a.includes('g5'), Object.keys(a).join(','));
  const seen = []; a.forEach((v, i) => seen.push(i + ':' + v)); out.push(seen.join(' '));
  out.push([...a].join(','), JSON.stringify(Object.getOwnPropertyDescriptor(a, 1) && Object.keys(Object.getOwnPropertyDescriptor(a, 1))));
  for (const v of a) out.push('of ' + v);
  for (const k in a) out.push('in ' + k);
  out.push(a.hasOwnProperty(1), 1 in a, a.propertyIsEnumerable(1));
  a.push(6); out.push(a.length, a[5]);
  out.push(a.pop(), a.length);
  const b = [1, 2, 3];
  Object.defineProperty(b, 0, { writable: false });
  b[0] = 9; out.push(b[0], JSON.stringify(Object.getOwnPropertyDescriptor(b, 0)));
  try { 'use strict'; (function () { 'use strict'; b[0] = 8; })(); out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
  Object.defineProperty(b, 1, { enumerable: false });
  out.push(Object.keys(b).join(','), Object.getOwnPropertyNames(b).join(','), JSON.stringify(b), b.join('-'));
  Object.defineProperty(b, 6, { value: 'far', configurable: false });
  out.push(b.length, JSON.stringify(b), Object.getOwnPropertyNames(b).join(','));
  b.length = 2; out.push(b.length, b[6]);
  try { (function () { 'use strict'; b.length = 0; })(); } catch (e) { out.push(e.constructor.name); }
  out.push(b.length);
  const c = [5, 1, 4];
  Object.defineProperty(c, 1, { get() { return 3; }, configurable: true });
  out.push(c.slice().sort().join(','), c.map((x) => x * 2).join(','), c.reverse === Array.prototype.reverse);
  try { c.sort(); out.push(c.join(',')); } catch (e) { out.push('sort ' + e.constructor.name); }
  const d = [1, 2, 3, 4];
  Object.defineProperty(d, 2, { get() { return 'G'; }, configurable: true });
  out.push(d.splice(1, 2).join(','), d.join(','), d.length, Object.getOwnPropertyDescriptor(d, 2) === undefined);
  const e = [1, 2, 3];
  Object.defineProperty(e, 1, { get() { return 'eg'; }, configurable: false });
  try { delete e[1]; out.push('del ' + (1 in e)); } catch (er) { out.push(er.constructor.name); }
  out.push(delete e[1]);
  try { e.pop(); e.pop(); out.push('popped'); } catch (er) { out.push('pop ' + er.constructor.name + ' ' + e.length); }
  Object.freeze(e); out.push(Object.isFrozen(e), Object.isSealed(e));
  const f = [1, 2, 3]; Object.seal(f);
  try { Object.defineProperty(f, 1, { get() {} }); } catch (er) { out.push(er.constructor.name); }
  Object.defineProperty(f, 0, { writable: false });
  out.push(Object.isFrozen(f), JSON.stringify(Object.getOwnPropertyDescriptor(f, 0)));
  Object.defineProperty(f, 1, { writable: false }); Object.defineProperty(f, 2, { writable: false });
  out.push(Object.isFrozen(f));
  const g = [1, 2, 3]; Object.preventExtensions(g); out.push(Object.isExtensible(g), Object.isSealed(g));
  try { Object.defineProperty(g, 5, { get() {} }); } catch (er) { out.push(er.constructor.name); }
  g.length = 1; out.push(g.length, Object.isSealed(g), Object.isFrozen(g));
  const h = [0, 1, 2];
  Object.defineProperty(h, 1, { get() { return 'H'; } });
  out.push(h.concat([9]).join(','), Array.from(h).join(','), h.at(1), h.findIndex((x) => x === 'H'), {...h}[1], Object.entries(h).join(';'), Object.values(h).join(','));
  return out.join(' | ');
}

function realHot() {
  const out = [];
  const a = [1, 2, 3, 4];
  let reads = 0;
  out.push(hot(a, 16), a.join(','));
  Object.defineProperty(a, 2, { get() { reads++; return 'acc'; }, set(v) { out.push('set ' + v); }, configurable: true });
  out.push(hot(a, 16), reads, a.join(','));
  const b = [10, 20, 30];
  Object.defineProperty(b, 0, { writable: false });
  out.push(hot(b, 9), b.join(','));
  const c = [1, 2, 3];
  Object.freeze(c);
  try { hot(c, 6); out.push('no throw'); } catch (e) { out.push(e.constructor.name, c.join(',')); }
  return out.join(' | ');
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
  console.log(realHot());
}
