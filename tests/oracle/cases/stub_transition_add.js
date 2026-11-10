// Adds made through the write stub's transition arm: objects built field by
// field by a plain function constructor and by setters, where the site's way 0
// holds the transition to the shape one key above the receiver's. Each case
// changes something the arm must notice: a setter or a read-only property
// appearing on the chain, a receiver that is a prototype, a frozen, sealed or
// non-extensible receiver, a key already present, an overflow slot.

function EE(fn, context, once) {
  this.fn = fn;
  this.context = context;
  this.once = once || false;
}
const listeners = [];
for (let i = 0; i < 6; i++) listeners.push(new EE(i, 'c' + i, i % 2 === 0));
console.log(JSON.stringify(listeners.map((l) => [l.fn, l.context, l.once, Object.keys(l).join('|')])));

// One function filling a plain object through a setter-style method.
function fill(o, a, b, c) {
  o.alpha = a;
  o.beta = b;
  o.gamma = c;
  return o;
}
const filled = [];
for (let i = 0; i < 4; i++) filled.push(fill({}, i, i * 2, 'g' + i));
console.log(JSON.stringify(filled));

// A setter appears on the prototype after the site is warm: the add must call it.
const proto = {};
let setterCalls = 0;
for (let i = 0; i < 3; i++) fill(Object.create(proto), i, i, i);
Object.defineProperty(proto, 'beta', { set(v) { setterCalls++; this._beta = v; }, get() { return this._beta; }, configurable: true });
const viaSetter = fill(Object.create(proto), 1, 2, 3);
console.log(setterCalls, Object.keys(viaSetter).join('|'), viaSetter.beta);

// A read-only property appears on the prototype: the add is refused (sloppy).
const proto2 = {};
for (let i = 0; i < 3; i++) fill(Object.create(proto2), i, i, i);
Object.defineProperty(proto2, 'gamma', { value: 'locked', writable: false });
const ro = fill(Object.create(proto2), 1, 2, 3);
console.log(Object.keys(ro).join('|'), ro.gamma);

// Strict mode: the same refusal is a TypeError.
function fillStrict(o, v) { 'use strict'; o.alpha = v; o.beta = v; o.gamma = v; return o; }
for (let i = 0; i < 3; i++) fillStrict({}, i);
try { fillStrict(Object.create(proto2), 9); console.log('no throw'); } catch (e) { console.log(e.constructor.name); }

// Frozen, sealed and non-extensible receivers of the warm shape's parent.
for (let i = 0; i < 3; i++) fill({}, 1, 2, 3);
const frozen = Object.freeze({});
const sealed = Object.seal({});
const nonExt = Object.preventExtensions({});
fill(frozen, 1, 2, 3);
fill(sealed, 1, 2, 3);
fill(nonExt, 1, 2, 3);
console.log(Object.keys(frozen).length, Object.keys(sealed).length, Object.keys(nonExt).length);
try { fillStrict(Object.preventExtensions({}), 1); console.log('no throw'); } catch (e) { console.log(e.constructor.name); }

// A receiver that is some object's prototype: the add must be seen by the child.
const parent = {};
const child = Object.create(parent);
for (let i = 0; i < 3; i++) fill({}, 0, 0, 0);
fill(parent, 'pa', 'pb', 'pc');
console.log(child.alpha, child.beta, child.gamma);

// Many keys: overflow slots past the inline ones.
function wide(o, n) {
  o.k0 = n; o.k1 = n; o.k2 = n; o.k3 = n; o.k4 = n; o.k5 = n; o.k6 = n; o.k7 = n;
  o.k8 = n; o.k9 = n; o.k10 = n; o.k11 = n; o.k12 = n; o.k13 = n; o.k14 = n; o.k15 = n;
  o.k16 = n; o.k17 = n; o.k18 = n; o.k19 = n;
  return o;
}
let sum = 0;
for (let i = 0; i < 5; i++) {
  const o = wide({}, i);
  for (const k in o) sum += o[k];
}
console.log(sum);

// Deleted-then-readded keys and receivers of other shapes at the same site.
const d = fill({}, 1, 2, 3);
delete d.beta;
fill(d, 4, 5, 6);
console.log(Object.keys(d).join('|'), d.alpha, d.beta, d.gamma);
const pre = { gamma: 'first' };
fill(pre, 7, 8, 9);
console.log(Object.keys(pre).join('|'));

// Symbols and numeric-string keys through a computed write in a loop.
const sym = Symbol('s');
function addKeys(o, ks) { for (const k of ks) o[k] = String(k === sym ? 'sym' : k); return o; }
for (let i = 0; i < 3; i++) addKeys({}, ['x', '1', sym, 'y']);
const ko = addKeys({}, ['x', '1', sym, 'y']);
console.log(Object.keys(ko).join('|'), ko[sym]);

// An instance of a class whose prototype has a setter for one of the keys.
class WithSetter { set beta(v) { this._b = v * 10; } }
for (let i = 0; i < 3; i++) fill({}, 1, 1, 1);
const ws = fill(new WithSetter(), 1, 2, 3);
console.log(Object.keys(ws).join('|'), ws._b);
