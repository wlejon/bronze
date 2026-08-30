// `o[k] = v` — the computed STORE, at every point where the answer is decided
// by something other than "write the slot the key names".
//
// The store cache (runtime/elem_ic.h) makes a warm (shape, key) pair a slot
// write with no walk, so every refusal, every accessor and every invalidation
// below is a chance for a cached answer to outlive the fact it stood on. Each
// scenario therefore WARMS the site first and only then changes the thing the
// entry depended on — a cache that filled and never re-checked would keep
// answering, and the pinned bytes are what that would break.
//
// The shape this was written for is three.js `BoxGeometry.buildPlane`:
// `vector[u] = x` with `u` a string parameter, so one site whose key is a
// different name on each call and whose receiver is one shape throughout.

function show(label, value) {
  console.log(label + ': ' + value);
}

// --- the three.js shape: one site, the key varying between calls ------------
function assign(target, name, value) {
  target[name] = value;
}
const vec = { x: 0, y: 0, z: 0 };
const components = ['x', 'y', 'z'];
for (let round = 0; round < 4; round++) {
  for (let i = 0; i < 3; i++) {
    assign(vec, components[i], round * 10 + i);
  }
}
show('key varies per call', vec.x + '/' + vec.y + '/' + vec.z);

// The same site with the receiver SHAPE varying instead, so the entry has to
// be about the shape as well as the key.
const oneField = { m: 0 };
const twoFields = { n: 0, m: 0 };
for (let i = 0; i < 4; i++) {
  assign(oneField, 'm', i);
  assign(twoFields, 'm', i * 3);
}
show('shape varies per call', oneField.m + '/' + twoFields.m);

// Two keys on ONE shape, alternating. An entry keyed on the shape alone would
// send each value to the other key's slot.
const pair = { p: 0, q: 0 };
for (let i = 0; i < 6; i++) {
  pair['p'] = i;
  pair['q'] = i * 100;
}
show('two keys one shape', pair.p + '/' + pair.q);

// A store that ADDS the property, over fresh objects of one shape: the
// transition every `new Vector3()` takes.
const added = [];
for (let i = 0; i < 5; i++) {
  const fresh = {};
  fresh['made'] = i;
  added.push(fresh.made);
}
show('transition add', added.join(','));

// --- accessors --------------------------------------------------------------
// An own setter runs on EVERY store, warm or not: a cached slot write would
// store into the pair's slot and never call it.
class Recorder {
  constructor() { this.seen = []; }
  set v(value) { this.seen.push(value); }
  get v() { return this.seen.length; }
}
const recorder = new Recorder();
for (let i = 0; i < 5; i++) {
  recorder['v'] = i;
}
show('own setter runs every time', recorder.seen.join(','));

// An INHERITED setter, reached by the same computed store.
const setterProto = {
  set doubled(value) { this.stored = value * 2; },
  get doubled() { return this.stored; }
};
const inheritsSetter = Object.create(setterProto);
for (let i = 1; i <= 3; i++) {
  inheritsSetter['doubled'] = i;
}
show('inherited setter', inheritsSetter.stored);

// A setter APPEARING on the prototype after the site is warm. The first five
// stores create own properties; the sixth must find the setter instead.
const lateProto = {};
const beforeSetter = [];
for (let i = 0; i < 5; i++) {
  const child = Object.create(lateProto);
  child['late'] = i;
  beforeSetter.push(child.late);
}
Object.defineProperty(lateProto, 'late', {
  set: function (value) { this.viaSetter = value * 10; },
  get: function () { return this.viaSetter; },
  configurable: true
});
const afterSetter = Object.create(lateProto);
afterSetter['late'] = 7;
show('proto setter appears', beforeSetter.join(',') + '|' + afterSetter.late);

// --- refusals ---------------------------------------------------------------
// Freezing AFTER the site is warm. Sloppy code discards the write
// (ECMA-262 10.1.9.2 -> 10.1.6.3 returns false, and 13.15.2 PutValue only
// throws for a strict reference).
const frozenLater = { a: 0 };
for (let i = 1; i <= 4; i++) {
  frozenLater['a'] = i;
}
const beforeFreeze = frozenLater.a;
Object.freeze(frozenLater);
frozenLater['a'] = 99;
show('freeze after warm', beforeFreeze + ',' + frozenLater.a);

// The same write in STRICT code throws instead (13.15.2 step 6.d).
function strictFrozenStore() {
  'use strict';
  const target = { a: 1 };
  target['a'] = 2;
  Object.freeze(target);
  try {
    target['a'] = 3;
    return 'no throw, a=' + target.a;
  } catch (e) {
    return e.name + ', a=' + target.a;
  }
}
show('freeze after warm strict', strictFrozenStore());

// A sealed object keeps its writable properties and refuses new ones
// (10.1.6.3 step 2.b: not extensible).
const sealed = { a: 1 };
Object.seal(sealed);
sealed['a'] = 2;
sealed['b'] = 9;
show('sealed', sealed.a + '/' + sealed.b);

const notExtensible = { a: 1 };
Object.preventExtensions(notExtensible);
notExtensible['b'] = 2;
show('preventExtensions', notExtensible.b);

// An inherited NON-WRITABLE data property refuses the write outright
// (10.1.9.2 step 2 reaches OrdinarySetWithOwnDescriptor with the PARENT's
// descriptor and returns false): no own property is created to shadow it.
const readOnlyProto = {};
const beforeReadOnly = [];
for (let i = 0; i < 4; i++) {
  const child = Object.create(readOnlyProto);
  child['w'] = i;
  beforeReadOnly.push(child.w);
}
Object.defineProperty(readOnlyProto, 'w', { value: 1, configurable: true });
const afterReadOnly = Object.create(readOnlyProto);
afterReadOnly['w'] = 5;
show('inherited nonwritable', beforeReadOnly.join(',') + '|' + afterReadOnly.w);

function strictInheritedReadOnly() {
  'use strict';
  const proto = {};
  Object.defineProperty(proto, 'w', { value: 1, configurable: true });
  const child = Object.create(proto);
  try {
    child['w'] = 5;
    return 'no throw';
  } catch (e) {
    return e.name;
  }
}
show('inherited nonwritable strict', strictInheritedReadOnly());

// --- receivers that are not plain objects -----------------------------------
// A String exotic object refuses its own index and `length` (10.4.3), and
// takes an ordinary name like any object.
const wrapper = new String('abc');
wrapper['0'] = 'Z';
show('String wrapper index', wrapper[0] + '/' + wrapper.length);
for (let i = 0; i < 4; i++) {
  wrapper['tag'] = i;
}
show('String wrapper name', wrapper.tag + '/' + wrapper[0]);

// An ARRAY key that is not a canonical index NAMES a property and leaves
// `length` alone.
const arr = [1, 2, 3];
for (let i = 0; i < 4; i++) {
  arr['tag'] = i;
}
show('array name key', arr.tag + '/' + arr.length + '/' + arr[0]);

// A Proxy's set trap runs on every store, warm or not (10.5.9).
const trapped = [];
const proxied = new Proxy({}, {
  set: function (target, key, value) {
    trapped.push(key + '=' + value);
    target[key] = value;
    return true;
  }
});
for (let i = 0; i < 3; i++) {
  proxied['q'] = i;
}
show('proxy set trap', trapped.join(',') + '/' + proxied.q);

// --- key kinds --------------------------------------------------------------
// A number key names a string property (7.1.19 ToPropertyKey), so `o[7]` and
// `o['7']` are one property.
const numberKeyed = {};
for (let i = 0; i < 4; i++) {
  numberKeyed[7] = i;
}
show('number key', numberKeyed[7] + '/' + numberKeyed['7']);

// A boolean key is a name too.
const boolKeyed = {};
boolKeyed[true] = 'T';
boolKeyed[false] = 'F';
show('boolean key', boolKeyed.true + '/' + boolKeyed.false);

// A symbol key is its own identity and never becomes a string.
const symbolKeyed = {};
const marker = Symbol('marker');
symbolKeyed[marker] = 5;
show('symbol key', symbolKeyed[marker] + '/' + symbolKeyed.marker);

// An OBJECT key runs `toString` before the store (7.1.19), and that is user
// code that must happen exactly once per store.
const conversions = [];
const objectKey = {
  toString: function () { conversions.push('called'); return 'made'; }
};
const objectKeyed = {};
for (let i = 0; i < 3; i++) {
  objectKeyed[objectKey] = i;
}
show('object key toString', objectKeyed.made + '/' + conversions.length);

// --- dictionary mode --------------------------------------------------------
// A delete moves the object to dictionary mode, where slot numbers cached
// against the old shape name nothing.
const deleted = { a: 1, b: 2, c: 3 };
for (let i = 0; i < 4; i++) {
  deleted['b'] = i;
}
delete deleted.a;
deleted['b'] = 42;
deleted['d'] = 8;
show('delete then store', deleted.b + '/' + deleted.a + '/' + deleted.d);

// A prototype SWAP after the site is warm.
const firstProto = { tag: 'first' };
const swapped = Object.create(firstProto);
for (let i = 0; i < 4; i++) {
  swapped['own'] = i;
}
Object.setPrototypeOf(swapped, { tag: 'second' });
swapped['own'] = 9;
show('proto swap', swapped.own + '/' + swapped.tag);
