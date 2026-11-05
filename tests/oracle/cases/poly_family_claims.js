// Polymorphic family claims (type feedback, docs/type-feedback.md "A2"): a
// property site whose receivers are instances of many subclasses of one base
// reads or writes a base field at the slot every subclass shares, behind one
// family guard. The receivers come out of an array, so the whole-program pass
// cannot see their classes and only the profile can claim the sites. Then
// every way of moving a receiver off its layout happens to receivers of the
// claimed sites, which must keep answering what the language says.
class Base {
  constructor(i) {
    this.id = i;
    this.name = 'n' + i;
    this.w = i + 0.5;          // a double slot
    this.cb = baseCb;          // a function-valued field
    this.tag = 'base';
  }
  describe() { return 'base:' + this.id; }
  shared() { return this.w * 2; }
}
function baseCb() { return 'baseCb'; }
function otherCb() { return 'otherCb'; }

class S0 extends Base { constructor(i) { super(i); this.s0 = 0; } }
class S1 extends Base {
  constructor(i) { super(i); this.s1a = 1; this.s1b = 2; }
  describe() { return 's1:' + this.id; }
}
class S2 extends Base { constructor(i) { super(i); this.s2 = 'two'; this.tag = 's2'; } }
class S3 extends Base {
  constructor(i) { super(i); this.s3a = 3; this.s3b = 3; this.s3c = 3; }
  shared() { return 'S3.shared ' + this.name; }
}
class S4 extends Base { constructor(i) { super(i); } }
class S5 extends S1 {
  constructor(i) { super(i); this.s5 = 5; }
  describe() { return 's5>' + super.describe(); }
}
class S6 extends Base { constructor(i) { super(i); this.s6 = [6]; } }
class S7 extends Base { constructor(i, extra) { super(i); if (extra) this.late = extra; this.s7 = 7; } }
class S8 extends S3 { constructor(i) { super(i); this.s8 = 8; } }
// Same names, other order and another root: no family begins it.
class Fake { constructor(i) { this.name = 'fake' + i; this.id = -i; this.w = 0; } describe() { return 'fake'; } }

const KINDS = [S0, S1, S2, S3, S4, S5, S6, S7, S8];
function makePool() {
  const pool = [];
  for (let i = 0; i < 18; i++) pool.push(new KINDS[i % KINDS.length](i, i % 4 === 0 ? 'x' : undefined));
  return pool;
}

function show(v) {
  if (typeof v === 'function') return 'fn:' + v.name;
  if (typeof v === 'object' && v !== null) return JSON.stringify(v);
  return String(v);
}
function readAll(pool) {
  let s = '';
  for (let i = 0; i < pool.length; i++) {
    const o = pool[i];
    s += show(o.id) + ',' + show(o.name) + ',' + show(o.w) + ',' + show(o.tag) + ';';
  }
  return s;
}
function writeW(pool, v) {
  for (let i = 0; i < pool.length; i++) pool[i].w = v + i;
}
function writeWStrict(pool, v) {
  'use strict';
  let errs = 0;
  for (let i = 0; i < pool.length; i++) {
    try { pool[i].w = v; } catch (e) { errs++; }
  }
  return errs;
}
function writeCb(o, f) { o.cb = f; }
// Stores only the first pool reaches, so the profile names its shapes.
function writeOwn(pool, v) {
  for (let i = 0; i < pool.length; i++) {
    const o = pool[i];
    o.w = v;
    o.tag = 't' + (i % 3);
  }
}
function callAll(pool) {
  let s = '';
  for (let i = 0; i < pool.length; i++) {
    const o = pool[i];
    s += o.describe() + '/' + show(o.shared()) + '/' + o.cb() + ';';
  }
  return s;
}
function hot(pool, rounds) {
  let r = '', c = '';
  for (let k = 0; k < rounds; k++) {
    r = readAll(pool);
    c = callAll(pool);
  }
  return r + '\n' + c;
}

let pool = makePool();
console.log('warm', hot(pool, 60));
writeW(pool, 100);
for (let k = 0; k < 40; k++) writeW(pool, 0.25);
console.log('written', readAll(pool));
for (let k = 0; k < 40; k++) writeOwn(pool, k + 0.75);
console.log('own writes', readAll(pool));

// 1. delete a base field on one receiver: dictionary mode.
delete pool[3].name;
// 2. redefine a base field as an accessor.
Object.defineProperty(pool[4], 'w', { get() { return 'getter-w'; }, set(v) { this.tag = 'set:' + v; }, configurable: true });
// 3. a new prototype for one receiver.
Object.setPrototypeOf(pool[5], { describe() { return 'swapped'; }, shared() { return 'swapped-shared'; } });
// 4. frozen.
Object.freeze(pool[6]);
console.log('mutated', hot(pool, 3));
writeOwn(pool, 1.5);
console.log('own writes mutated', readAll(pool));
writeW(pool, 7);
console.log('sloppy writes', readAll(pool));
console.log('strict write errors', writeWStrict(pool, 9), readAll(pool));

// 5. a getter on the base prototype: own data still wins, a deleted field
// now finds it.
Object.defineProperty(Base.prototype, 'id', { get() { return 'proto-id'; }, configurable: true });
Object.defineProperty(Base.prototype, 'name', { get() { return 'proto-name'; }, configurable: true });
console.log('proto getters', readAll(pool));
delete pool[0].id;
console.log('deleted id', readAll(pool));
// (Class bodies are strict: a getter-only `id` up the chain would make the
// constructors' `this.id = i` throw, so they go before more are built.)
delete Base.prototype.id;
delete Base.prototype.name;
console.log('getters gone', readAll(pool));

// 6. a receiver that becomes a prototype; a store to its function field
// must reach the method calls its heir makes. One store site sees the
// prototype among the pool's objects, so the site is claimed and its guard
// meets the prototype's shape (the runtime never stamps it: a bare store
// would leave the heir's method cache naming the old function).
function heirRound(pool, parent, heir) {
  let s = '';
  for (let k = 0; k < 160; k++) {
    const toParent = k % 4 === 3;
    writeCb(toParent ? parent : pool[k % pool.length], (k >> 2) % 2 ? otherCb : baseCb);
    if (toParent) s += callHeir(heir)[0];
  }
  return s;
}
function callHeir(o) { return o.cb() + ' ' + o.describe() + ' ' + o.name; }
const fresh = makePool();
const parent = fresh[1];
const heir = Object.create(parent);
console.log('heir', heirRound(fresh, parent, heir), callHeir(heir));
const parent2 = fresh[4];
const heir2 = Object.create(parent2);
console.log('heir2', heirRound(fresh, parent2, heir2), callHeir(heir2));

// 7. the double slot takes a string, then an object.
pool[2].w = 'str';
pool[7].w = { boxed: 1 };
console.log('generalized', readAll(pool));
writeOwn(pool, 'own-str');
console.log('own generalized', readAll(pool));

// 8. a method replaced on the base prototype, and an own method on one.
Base.prototype.shared = function () { return 'replaced ' + this.tag; };
pool[8].describe = function () { return 'own-describe'; };
console.log('methods', hot(pool, 3));

// 9. foreign receivers mixed in: another class, a literal laid out like a
// Base, a string, an array, a number.
const mixed = makePool();
mixed.push(new Fake(1));
mixed.push({ id: 'lit', name: 'literal', w: 1.25, cb: baseCb, tag: 'lit' });
for (let k = 0; k < 40; k++) readAll(mixed);
console.log('mixed', readAll(mixed));
console.log('primitives', readAll(['abc', [1, 2], 42, true]));

// 10. appends after construction, then deletes and re-adds.
const grown = makePool();
for (let i = 0; i < grown.length; i++) {
  for (let j = 0; j < 12; j++) grown[i]['extra' + j] = j;
}
for (let k = 0; k < 40; k++) writeW(grown, 0.5);
console.log('grown', readAll(grown));
delete grown[9].w;
grown[9].w = 'readded';
grown[10].tag = undefined;
writeW(grown, 3);
console.log('readded', readAll(grown));
