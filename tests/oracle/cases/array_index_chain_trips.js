// Every way an index-keyed property can come to sit on an array's prototype
// chain, each checked on a fresh chain with a hole read, an `in`, a read past
// the length and a store into a hole: an index added to an existing
// prototype (plain assignment, defineProperty, defineProperties, a class's
// static accessor, Object.assign, Reflect.set/defineProperty, __defineGetter__)
// and a prototype that already held one when it was put in the chain
// (setPrototypeOf, `__proto__ =`, Object.create, a class extending Array
// whose prototype has an index accessor, a typed array, an arguments object,
// another array, a Proxy). Runs each probe twice so the
// second sees code warmed on the first. (Not here: a String wrapper as a
// prototype, whose indices bronze does not yet answer from any chain, and
// Array.prototype's own indices, which bronze refuses to create.)
function probe(a) {
  let r = '';
  for (let rep = 0; rep < 2; rep++) {
    r = String(a[1]) + ' ' + (1 in a) + ' ' + String(a[4]) + ' ' + (4 in a) + ' len=' + a.length;
  }
  return r;
}
function holey() { const a = [10, , 30]; return a; }
let out = '';
function t(name, f) {
  try { out += name + ': ' + f() + '\n'; } catch (e) { out += name + ': threw ' + e.constructor.name + '\n'; }
}
function onProto(setup) {
  const p = Object.create(Array.prototype);
  setup(p);
  const a = holey();
  Object.setPrototypeOf(a, p);
  return a;
}
t('assign later', () => { const p = Object.create(Array.prototype); const a = holey(); Object.setPrototypeOf(a, p); const before = probe(a); p[1] = 'P1'; p[4] = 'P4'; return before + ' | ' + probe(a); });
t('defineProperty', () => probe(onProto((p) => Object.defineProperty(p, 1, { value: 'D1' }))));
t('defineProperties', () => probe(onProto((p) => Object.defineProperties(p, { 1: { value: 'DS1' }, 4: { get() { return 'DS4'; } } }))));
t('Object.assign', () => probe(onProto((p) => Object.assign(p, { 1: 'A1', 4: 'A4' }))));
t('Reflect.set', () => probe(onProto((p) => Reflect.set(p, '1', 'R1'))));
t('Reflect.defineProperty', () => probe(onProto((p) => Reflect.defineProperty(p, 4, { get: () => 'RD4' }))));
t('__defineGetter__', () => probe(onProto((p) => p.__defineGetter__('1', () => 'DG1'))));
t('string key 1', () => probe(onProto((p) => { p['1'] = 'S1'; })));
t('non-index keys', () => probe(onProto((p) => { p['01'] = 'x'; p['1.0'] = 'y'; p['-1'] = 'z'; p['4294967295'] = 'w'; })));
t('setPrototypeOf obj', () => { const a = holey(); Object.setPrototypeOf(a, { 1: 'O1', 4: 'O4' }); return probe(a); });
t('__proto__ =', () => { const a = holey(); a.__proto__ = { 1: 'U1' }; return probe(a) + ' ' + (Object.getPrototypeOf(a).hasOwnProperty('1')); });
t('Object.create chain', () => { const base = { 4: 'C4' }; const mid = Object.create(base); const a = holey(); Object.setPrototypeOf(a, mid); return probe(a); });
t('deep later', () => { const base = {}; const mid = Object.create(base); const a = holey(); Object.setPrototypeOf(a, mid); const b = probe(a); base[1] = 'deep'; return b + ' | ' + probe(a); });
t('class static accessor', () => {
  class P { static get 1() { return 'static1'; } }
  const a = holey(); Object.setPrototypeOf(a, P); return probe(a);
});
t('class proto accessor', () => {
  class A extends Array { get 1() { return 'cls1'; } }
  const a = new A(3); a[0] = 'x'; return probe(a);
});
t('class proto method numeric', () => {
  class A extends Array { 4() { return 'm4'; } }
  const a = A.from([1, , 3]); return typeof a[4] + ' ' + (4 in a) + ' ' + String(a[1]);
});
t('typed array proto', () => { const a = holey(); Object.setPrototypeOf(a, new Int8Array([7, 8, 9, 10, 11])); return probe(a); });
t('arguments proto', () => { const ar = (function () { return arguments; })('g0', 'g1'); const a = holey(); Object.setPrototypeOf(a, ar); return probe(a); });
t('array proto', () => { const a = holey(); Object.setPrototypeOf(a, ['q0', 'q1', 'q2', 'q3', 'q4']); return probe(a); });
t('proxy proto', () => {
  let log = '';
  const px = new Proxy({}, {
    get(tg, k) { if (typeof k === 'string') log += 'g' + k + ','; return k === '1' ? 'PX1' : undefined; },
    has(tg, k) { if (typeof k === 'string') log += 'h' + k + ','; return k === '1'; },
  });
  const a = holey(); Object.setPrototypeOf(a, px);
  const r = String(a[1]) + ' ' + (1 in a) + ' ' + String(a[4]) + ' ' + (4 in a);
  return r + ' ' + log;
});
t('store into hole, setter', () => {
  let log = '';
  const a = onProto((p) => Object.defineProperty(p, 1, { set(v) { log += 'set' + v; }, get() { return 'g'; } }));
  a[1] = 5; a[1] = 6;
  return log + ' own=' + Object.hasOwn(a, 1) + ' ' + a[1];
});
t('store past end, non-writable', () => {
  const a = onProto((p) => Object.defineProperty(p, 4, { value: 'ro', writable: false }));
  a[4] = 'w';
  return Object.hasOwn(a, 4) + ' ' + a[4] + ' ' + a.length;
});
t('store past end, non-writable strict', () => {
  'use strict';
  const a = onProto((p) => Object.defineProperty(p, 4, { value: 'ro', writable: false }));
  try { a[4] = 'w'; return 'no throw'; } catch (e) { return e.constructor.name + ' ' + a.length; }
});
t('writable inherited data shadows', () => {
  const a = onProto((p) => { p[1] = 'inh'; });
  a[1] = 'own';
  return Object.hasOwn(a, 1) + ' ' + a[1] + ' ' + Object.getPrototypeOf(a)[1];
});
t('delete restores', () => {
  const p = Object.create(Array.prototype); p[1] = 'P'; const a = holey(); Object.setPrototypeOf(a, p);
  const b = probe(a); delete p[1]; return b + ' | ' + probe(a);
});
t('Object.prototype', () => { Object.prototype[4] = 'OP4'; const r = probe(holey()) + ' ' + [1][4]; delete Object.prototype[4]; return r + ' | ' + probe(holey()); });
console.log(out);
