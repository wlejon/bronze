// One receiver's shape, read once and reused by every guard on it that
// follows (il2mir/il_shape_memo.cpp), must be read again after anything that
// can change it.
//
// The memo crosses a static site on ANOTHER receiver as one would cross a
// load, but that site's miss runs the ordinary access, which may run a
// getter or a setter, which may do anything to the receiver the memo is
// about. Each method below reads `this`, reads `src.g` (a static site of its
// own: `src` is one object of one class), then reads `this` again. They are
// warmed with plain data first, so every cell is published and the second
// read of `this` has a memo to reuse; then `src.g` becomes an accessor, so
// that site misses, and the accessor does one of the things that move a
// shape to `this`. Every line must read as the ordinary [[Get]] / [[Set]]
// would.

const N = 2000;

class Src {
  constructor() {
    this.g = 1;
  }
}
const src = new Src();
// What src.g's getter does before it answers 7.
let act = () => {};

class V {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  len() {
    return this.x + this.y;
  }
  // this.x, src.g, this.x, this.y.
  pair() {
    const p = this.x;
    const q = src.g;
    const r = this.x;
    const s = this.y;
    return p + ':' + q + ':' + r + ':' + s;
  }
  // this.x = 5, src.g, this.x = 6, then read back. Class code is strict,
  // so a write the object refuses throws (and `run` reports it).
  writes() {
    this.x = 5;
    const q = src.g;
    this.x = 6;
    return q + ':' + this.x;
  }
  // An inlined method, src.g, the method again.
  methods() {
    const p = this.len();
    const q = src.g;
    const r = this.len();
    return p + ':' + q + ':' + r;
  }
  // A branch between the reads, so the memo crosses into a one-edge block
  // (on a strict compare, which runs no code of its own).
  branchy() {
    const p = this.x;
    const q = src.g;
    if (q === 7) return p + ':' + this.x + ':' + this.y;
    return 'neg:' + this.x;
  }
  // A call between the reads, and no static site.
  calls() {
    const p = this.x;
    act();
    const r = this.x;
    return p + ':' + r;
  }
}

// A receiver whose own getter reshapes it: the same object on both sides.
// Every Self is given an accessor `g` before it is read, so the cell of
// `this.x` is published for that shape and the miss is `this.g`'s alone.
class Self {
  constructor() {
    this.x = 1;
    this.g = 2;
  }
  read() {
    const p = this.x;
    const q = this.g;
    const r = this.x;
    return p + ':' + q + ':' + r;
  }
}

// Element stores fed by static reads (one guard for the whole run).
class P3 {
  constructor() {
    this.x = 1;
    this.y = 2;
  }
}
const pp = new P3();
class M {
  constructor() {
    this.e = [0, 0, 0];
  }
  fill() {
    const te = this.e;
    te[0] = pp.x;
    te[1] = pp.y;
    te[2] = pp.x;
    return te;
  }
}

let selfAct = () => {};
function oddGet() {
  selfAct(this);
  return 3;
}
function makeSelf() {
  const o = new Self();
  Object.defineProperty(o, 'g', { get: oddGet });
  return o;
}

let warm = 0;
for (let i = 0; i < N; i++) {
  const v = new V(1, 2);
  warm += v.pair().length + v.writes().length + v.methods().length + v.branchy().length + v.calls().length;
  warm += makeSelf().read().length;
  warm += new M().fill()[2];
}
console.log('warm', warm);

Object.defineProperty(src, 'g', { get() { act(); return 7; } });
let b;
function run(method, what) {
  b = new V(1, 2);
  act = what;
  let out;
  try {
    out = b[method]();
  } catch (e) {
    out = e.constructor.name + ':' + b.x;
  }
  act = () => {};
  return out;
}

// 1. A property turned into an accessor.
console.log('accessor', run('pair', () => Object.defineProperty(b, 'x', { get() { return 77; } })));

// 2. A property deleted: undefined now, not the slot's old value.
console.log('delete', run('pair', () => { delete b.x; }));

// 3. Deleted and re-added: x is last now, and y's slot is no longer 1.
console.log('readd', run('pair', () => { delete b.x; b.x = 9; }));

// 4. y deleted and a prototype given that has one.
console.log('proto', run('pair', () => { delete b.y; Object.setPrototypeOf(b, { y: 'py' }); }));

// 5. A slot holding numbers given a string.
console.log('widen', run('pair', () => { b.x = 'wide'; }));

// 6. Frozen between two writes: the second throws and changes nothing.
console.log('freeze', run('writes', () => Object.freeze(b)));

// 7. x made read-only between two writes.
console.log('readonly', run('writes', () => Object.defineProperty(b, 'x', { writable: false })));

// 8. x turned into a setter between two writes.
let seen = '';
console.log('setter', run('writes', () => Object.defineProperty(b, 'x', {
  get() { return 'got'; },
  set(v) { seen += v; },
})), seen);

// 9. An own `len` shadowing the method.
console.log('shadow', run('methods', () => { b.len = () => -2; }));

// 10. The prototype's method replaced: this's shape is the same, the method
// is not.
const oldLen = V.prototype.len;
console.log('patch', run('methods', () => { V.prototype.len = function () { return this.x * 10; }; }));
V.prototype.len = oldLen;

// 11. Across the branch: x deleted, y redefined.
console.log('branch', run('branchy', () => { delete b.x; Object.defineProperty(b, 'y', { value: 'dy' }); }));

// 12. A call between two reads: the memo is forgotten at it.
console.log('call', run('calls', () => Object.defineProperty(b, 'x', { get() { return 77; } })));

// 13. The SAME receiver: its own getter deletes x and adds it back last.
const odd = makeSelf();
selfAct = (o) => { delete o.x; o.x = 'back'; };
console.log('self', odd.read());

// 14. A receiver that is no plain object at all: a Proxy, then an Array
// with named properties. Neither may match a cell, before or after src.g.
const log = [];
const prox = new Proxy({ x: 'px', y: 'py' }, { get(t, k) { log.push(k); return t[k]; } });
console.log('proxy', V.prototype.pair.call(prox), log.join(','));
const arr = [];
arr.x = 'ax';
arr.y = 'ay';
console.log('array', V.prototype.pair.call(arr));

// 15. The element-store run whose static read misses: the getter on pp.x
// grows the array between the stores.
let k = 0;
const m = new M();
Object.defineProperty(pp, 'x', { get() { m.e.push('g'); return ++k; }, configurable: true });
console.log('fill', m.fill().join(','));
// The same run with a getter that freezes the array: the first store throws
// (strict class code) and none lands.
const m2 = new M();
Object.defineProperty(pp, 'x', { get() { Object.freeze(m2.e); return 'f'; } });
let threw = '';
try {
  m2.fill();
} catch (e) {
  threw = e.constructor.name;
}
console.log('fill frozen', threw, m2.e.join(','));
