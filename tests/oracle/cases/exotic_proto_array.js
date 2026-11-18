// An ARRAY as a prototype (runtime/rt_exotic_link.cpp): Object.create of one,
// Object.setPrototypeOf onto one, an object literal's __proto__. Its elements,
// `length` and named properties are inherited: read, `in`, for-in, a spread
// through the inherited iterator, Array.prototype methods over the heir. A
// later push, truncation or element write on the array is seen through the
// chain; a hole falls through to Array.prototype. A write to the heir makes
// an own property, unless the inherited element is an accessor (its setter
// runs with the heir as `this`) or non-writable (refused: silently, or a
// TypeError in strict code). `super` reads through an array.
const out = [];
const show = (...xs) =>
  out.push(xs.map((x) => (x === undefined ? 'u' : typeof x === 'string' ? x : JSON.stringify(x))).join(' '));

const arr = [7, 8];
const o = Object.create(arr);
show('read', o[0], o[1], o[2], o.length, o['1'], o[1.5]);
show('kind', Array.isArray(o), Object.getPrototypeOf(o) === arr, o instanceof Array, typeof o.push);
show('in', 0 in o, 1 in o, 2 in o, 'length' in o, 'push' in o);
show('own', o.hasOwnProperty(0), Object.keys(o), JSON.stringify(o), Object.getOwnPropertyDescriptor(o, 0));
const keys = [];
for (const k in o) keys.push(k);
show('for-in', keys);
show('spread', [...o], Array.prototype.join.call(o, '-'), Array.prototype.map.call(o, (x) => x * 2));

arr.push(9);
show('after push', o[2], o.length, 2 in o);
arr.length = 1;
show('after truncate', o[0], o[1], o.length, 1 in o);
arr[0] = 70;
show('after write', o[0]);

o[0] = 'own';
o.length = 10;
show('heir writes', o[0], arr[0], o.length, arr.length, Object.keys(o));
delete o[0];
show('after delete', o[0], delete o[0], o[0]);

// Builtins are frozen in bronze, so the level under the holey array is an
// ordinary object that inherits from Array.prototype.
const under = Object.create(Array.prototype);
under[0] = 'AP';
const holey = [, 2];
Object.setPrototypeOf(holey, under);
const h = Object.create(holey);
show('hole', h[0], 0 in h, h[1], h.join === Array.prototype.join);
delete under[0];
show('hole gone', h[0], 0 in h);

const acc = [1, 2, 3];
Object.defineProperty(acc, 0, {
  get() { return 'get ' + this.tag; },
  set(v) { this.got = v; },
  configurable: true,
  enumerable: true,
});
const a2 = Object.create(acc);
a2.tag = 'T';
show('accessor', a2[0], acc[0]);
a2[0] = 5;
show('setter', a2.got, a2.hasOwnProperty(0), acc.got);

const ro = [1, 2];
Object.defineProperty(ro, 0, { writable: false });
const r2 = Object.create(ro);
r2[0] = 'x';
show('read-only sloppy', r2[0], r2.hasOwnProperty(0));
try {
  (function () { 'use strict'; r2[0] = 'y'; })();
  show('read-only strict', 'no throw');
} catch (e) {
  show('read-only strict', e.constructor.name);
}
r2[1] = 'w';
show('writable', r2[1], ro[1]);

const p = {};
Object.setPrototypeOf(p, [5]);
show('setPrototypeOf', p[0], p.length, p.map === Array.prototype.map);

const lit = { __proto__: [41, 42], m() { return [super[1], super.length]; } };
show('super', lit.m(), lit[0]);

const inner = [, 'b'];
Object.setPrototypeOf(inner, ['A', 'B', 'C']);
show('array on array', inner[0], inner[1], inner[2], inner.length, 2 in inner);

const deep = Object.create(Object.create([3, 4]));
show('two links up', deep[1], deep.length, 1 in deep);

const sym = Symbol('s');
const tagged = [1];
tagged[sym] = 'symval';
tagged.named = 'nm';
const t2 = Object.create(tagged);
show('named', t2.named, t2[sym], 'named' in t2, sym in t2);

console.log(out.join('\n'));
