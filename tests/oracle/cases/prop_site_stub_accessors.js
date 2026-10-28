// Property reads and writes that no inference proved monomorphic, taken
// through the site's cache: own data, absent keys, a prototype's data and
// accessors, an object's own accessors, and every way the cached answer can
// stop being the right one while the site keeps running.

class Base {
  constructor(v) { this._v = v; this.log = []; }
  get v() { return this._v * 2; }
  set v(x) { this._v = x + 1; }
  get args() { return arguments.length; }
  set args(x) { this.log.push(arguments.length + ':' + arguments[0]); }
  get self() { return this; }
  set dflt(x = 5) { this._d = x; }
  get dflt() { return this._d; }
  get onlyGet() { return 'g'; }
  set onlySet(x) { this._s = x; }
}
Base.prototype.konst = 42;
Base.prototype.method = function () { return 'm' + this._v; };

class Derived extends Base {
  constructor(v) { super(v); this.extra = true; }
}

class Deep extends Derived {}

// Literal objects of several shapes, so the reads below are no proven shape.
function shapes(n) {
  const out = [];
  for (let i = 0; i < n; i++) {
    const k = i % 5;
    if (k === 0) out.push({ a: i, b: 'x' + i });
    else if (k === 1) out.push({ b: 'y' + i, a: i * 2 });
    else if (k === 2) out.push({ z: 1, a: i * 3, b: null });
    else if (k === 3) out.push({ q: 0, r: 0, s: 0, t: 0, u: 0, a: -i, b: [i] });
    else out.push(Object.create({ a: 'proto-a', b: 'proto-b' }));
  }
  return out;
}

function readAll(list) {
  let acc = 0;
  const seen = [];
  for (let i = 0; i < list.length; i++) {
    const o = list[i];
    const a = o.a;
    acc += typeof a === 'number' ? a : a.length;
    seen.push(String(o.b));
    if (o.missing !== undefined) seen.push('present?');
  }
  return acc + ' ' + seen.slice(0, 8).join(',') + ' ' + seen.length;
}

const list = shapes(60);
console.log('own/proto data', readAll(list), readAll(list));

// Writes through a site that sees several shapes, a reference value each time
// (the barrier) and a number.
function writeAll(list, val) {
  for (let i = 0; i < list.length; i++) {
    list[i].a = val;
    list[i].b = { n: i };
  }
}
writeAll(list, 7);
writeAll(list, 'seven');
let objs = 0;
for (const o of list) if (o.a === 'seven' && o.b.n >= 0) objs++;
console.log('writes', objs, list[4].a, Object.getPrototypeOf(list[4]).a);

// Accessors on the prototype (depth 1), its prototype (depth 2, 3), and the
// receiver's own (depth 0), read and written by one site each.
function accessors(list) {
  const out = [];
  for (let r = 0; r < 3; r++) {
    for (let i = 0; i < list.length; i++) {
      const o = list[i];
      o.v = i + r;
      out.push(o.v);
      o.args = r;
      out.push(o.args);
      out.push(o.self === o);
      o.dflt = undefined;
      out.push(o.dflt);
      out.push(o.konst);
      out.push(typeof o.method === 'function' ? o.method() : 'no');
    }
  }
  return out.join(' ');
}
const recv = [new Base(1), new Derived(2), new Deep(3), new Base(4)];
const own = { _v: 9, log: [], konst: 'own', method() { return 'own-m'; } };
Object.defineProperty(own, 'v', { get() { return this._v * 10; }, set(x) { this._v = x - 1; } });
Object.defineProperty(own, 'args', { get: function () { return arguments.length; }, set: function () { this.log.push(arguments.length); } });
Object.defineProperty(own, 'self', { get() { return this; } });
Object.defineProperty(own, 'dflt', { get() { return this._d; }, set(x = 6) { this._d = x; } });
recv.push(own);
console.log('accessors', accessors(recv));
console.log('setter args', recv[0].log.join(','), own.log.join(','));

// Accessor-only halves, sloppy and strict.
function halves(o) {
  o.onlyGet = 'ignored';
  const g = o.onlyGet;
  const s = o.onlySet;
  o.onlySet = 's';
  return g + '/' + s + '/' + o._s;
}
function strictHalves(o) {
  'use strict';
  try { o.onlyGet = 1; return 'no throw'; } catch (e) { return e.constructor.name; }
}
let h = '';
for (let i = 0; i < 4; i++) h += halves(recv[i % 3]) + ' ' + strictHalves(recv[i % 3]) + ';';
console.log('halves', h);

// The getter replaced after the site cached it, then the prototype swapped.
function readV(arr) { let s = ''; for (const o of arr) s += o.v + ','; return s; }
const swap = [new Base(1), new Derived(2), new Base(3)];
console.log('before', readV(swap), readV(swap));
Object.defineProperty(Base.prototype, 'v', { get() { return 'new-' + this._v; }, set(x) { this._v = x; }, configurable: true });
console.log('replaced', readV(swap));
Object.setPrototypeOf(swap[0], { get v() { return 'swapped'; } });
console.log('swapped', readV(swap));
Object.defineProperty(Derived.prototype, 'v', { value: 'data-now', writable: true, configurable: true });
console.log('shadowed by data', readV(swap));
delete Derived.prototype.v;
console.log('unshadowed', readV(swap));

// Native and bound getters, a getter that throws, and one that recurses.
const odd = [{}, { k: 1 }, { k: 2, j: 3 }];
for (const o of odd) {
  Object.defineProperty(o, 'tag', { get: Object.prototype.toString });
  Object.defineProperty(o, 'bound', { get: function () { return this.k; }.bind({ k: 'bound' }) });
}
let t = '';
for (let r = 0; r < 3; r++) for (const o of odd) t += o.tag + o.bound + ' ';
console.log('odd getters', t);

class Thrower { get boom() { throw new RangeError('boom ' + this.n); } constructor(n) { this.n = n; } }
const throwers = [new Thrower(1), { n: 2, get boom() { throw new TypeError('own ' + this.n); } }];
let caught = '';
for (let r = 0; r < 3; r++) {
  for (const o of throwers) {
    try { caught += o.boom; } catch (e) { caught += e.name + ':' + e.message + ' '; }
  }
}
console.log('throwing', caught);

class Tree {
  constructor(d) { this.d = d; this.kid = d > 0 ? new Tree(d - 1) : null; }
  get depth() { return this.kid === null ? 0 : 1 + this.kid.depth; }
}
const trees = [new Tree(5), { kid: null }, new Tree(2)];
Object.defineProperty(trees[1], 'depth', { get() { return -1; } });
let ds = '';
for (let r = 0; r < 3; r++) for (const o of trees) ds += o.depth + ' ';
console.log('recursive', ds);

// A setter that allocates before it reads its argument back through
// `arguments`, with an object for the value.
class Hoard {
  constructor() { this.kept = null; this.junk = null; }
  set item(v) {
    const junk = [];
    for (let i = 0; i < 200; i++) junk.push({ i, s: 'j' + i });
    this.junk = junk;
    this.kept = arguments[0];
  }
  get item() { return this.kept; }
}
const hoards = [new Hoard(), new Hoard(), Object.assign(Object.create(Hoard.prototype), { kept: null, junk: null, other: 1 })];
let hs = 0;
for (let r = 0; r < 20; r++) {
  for (const o of hoards) {
    o.item = { r, label: 'item' + r };
    hs += o.item.r + o.item.label.length + o.junk.length;
  }
}
console.log('hoard', hs);

// A prototype in dictionary mode, and a frozen one.
class Dict { get g() { return 'dict-g'; } m1() {} m2() {} }
delete Dict.prototype.m1;
const frozenProto = Object.freeze({ get g() { return 'frozen-g'; }, d: 'frozen-d' });
const mixed = [new Dict(), Object.create(frozenProto), new Dict(), Object.create(frozenProto)];
let ms = '';
for (let r = 0; r < 3; r++) for (const o of mixed) ms += o.g + ':' + o.d + ' ';
console.log('dict/frozen', ms);

// Frozen and sealed receivers through a write site, sloppy and strict.
function put(o, v) { o.a = v; return o.a; }
function putStrict(o, v) { 'use strict'; try { o.a = v; return o.a; } catch (e) { return e.name; } }
const fz = [Object.freeze({ a: 1 }), { a: 2 }, Object.seal({ a: 3 }), Object.preventExtensions({ b: 4 })];
let ps = '';
for (let r = 0; r < 3; r++) for (const o of fz) ps += put(o, r * 10) + '|' + putStrict(o, r * 10 + 1) + ' ';
console.log('frozen writes', ps);

// Primitive receivers and null at the same sites.
function lens(xs) {
  let s = '';
  for (const x of xs) { try { s += x.length + ','; } catch (e) { s += e.name + ','; } }
  return s;
}
console.log('primitives', lens(['abc', [1, 2], { length: 'L' }, new Uint8Array(3), 'é', null, undefined]));

// One receiver at a time through each site, so the site's cache holds its
// answer and every pass after the first is answered from it: a prototype
// accessor, a deeper one, an own one, and an allocating setter.
function sweep(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    o.v = i;
    s += o.v;
    o.dflt = i % 2 === 0 ? undefined : i;
    s += o.dflt;
    s += o.self === o ? 1 : 0;
    s += o.konst === 42 ? 1 : 0;
  }
  return s;
}
const solo = [new Base(0), new Deep(0), new Derived(0)];
Object.defineProperty(Base.prototype, 'v', { get() { return this._v * 3; }, set(x) { this._v = x + 2; }, configurable: true });
let sw = '';
for (const o of solo) sw += sweep(o, 50) + ',';
sw += sweep(own, 50);
console.log('sweep', sw);
const oneHoard = new Hoard();
function hoardLoop(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) { o.item = { i, tag: 'h' + i }; s += o.item.tag.length + o.item.i + o.junk[199].i; }
  return s;
}
console.log('hoard loop', hoardLoop(oneHoard, 60));
// The getter changed in the middle of a sweep, by the getter itself.
class Flip { constructor() { this.n = 0; } get f() { this.n++; if (this.n === 5) Object.defineProperty(Flip.prototype, 'f', { get() { return 'after'; }, configurable: true }); return 'before'; } }
function flips(o) { const out = []; for (let i = 0; i < 8; i++) out.push(o.f); return out.join(','); }
console.log('flip', flips(new Flip()));
