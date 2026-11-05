// S2 layout facts (docs/aot-specialization.md): a receiver's static-slot
// guard, asked once, answers its later reads and writes across calls while
// the layout epoch holds. Every way S1 lists of breaking a layout happens
// here inside a callee (or a getter) BETWEEN two covered accesses of the
// same receiver, across blocks, loops and a try; each must be seen.
class M {
  constructor(k) {
    this.a = k; this.b = k + 0.5; this.c = 2; this.d = 3;
    this.e = 4.25; this.f = 5; this.g = 6.5; this.h = 7;   // slots 4..7: out of line
  }
  get trigger() { mutate(this, 1); return 0; }
}
class N extends M {
  constructor(k) { super(k); this.n = 9; }
}
const KEYS = JSON.parse('["a","b","c","d","e","f","g","h","n"]');
let mode = 0;
let deep = false;
const log = [];
function mutate(o, depth) {
  switch (mode) {
    case 1: Reflect.deleteProperty(o, KEYS[5]); break;                     // delete an out-of-line slot
    case 2: Reflect.defineProperty(o, KEYS[1], { get() { return 'acc'; } }); break;  // redefine as accessor
    case 3: Object.freeze(o); break;
    case 4: Object.setPrototypeOf(o, { zz: 1 }); break;
    case 5: Reflect.set(o, KEYS[6], 'wide'); break;                         // double slot -> boxed
    case 6: for (let i = 0; i < 24; i++) Reflect.set(o, 'p' + i, i); break; // appends: overflow reallocated
    case 7: log.push(Object.create(o).h); break;                           // o becomes a prototype
    case 8: Object.preventExtensions(o); Reflect.set(o, 'late', 1); break;
    case 9: Reflect.deleteProperty(o, KEYS[0]); Reflect.set(o, KEYS[0], 'readded'); break;
    case 10: Object.seal(o); break;
    case 11: Reflect.defineProperty(o, KEYS[7], { value: 77, writable: false }); break;
    case 12: Reflect.set(o, KEYS[4], { boxed: true }); break;              // double slot -> object
  }
}
function poke(o) {
  if (deep) return pokeDeeper(o);
  if (mode !== 0) mutate(o, 0);
  return 0;
}
function pokeDeeper(o) { return mode !== 0 ? (mutate(o, 1), 0) : 0; }
function show(v) { return typeof v === 'object' && v !== null ? JSON.stringify(v) : String(v); }

// Straight line: reads on both sides of calls.
function straight(o) {
  let s = show(o.a);
  poke(o);
  s += ' ' + show(o.f) + ' ' + show(o.g);
  poke(o);
  s += ' ' + show(o.b) + ' ' + show(o.h) + ' ' + show(o.e);
  return s;
}
// Across blocks and a loop: the anchor reads before the branch, members in
// the arms, the loop body and after the join.
function blocks(o, flip) {
  let s = show(o.c);
  if (flip) { poke(o); s += ' T' + show(o.d) + show(o.f); }
  else { poke(o); s += ' F' + show(o.g) + show(o.b); }
  for (let i = 0; i < 3; i++) {
    poke(o);
    s += ' L' + show(o.h) + show(o.a);
  }
  poke(o);
  return s + ' E' + show(o.e) + show(o.c);
}
// Covered stores of numbers after a call: a frozen, sealed or read-only
// receiver must refuse them as the ordinary store does (module code is
// strict).
function stores(o, k) {
  let s = show(o.a);
  poke(o);
  try {
    o.b = k * 0.5;
    o.f = k + 0.25;
    poke(o);
    o.h = k * 2;
    o.e = k / 4;
  } catch (e) {
    s += ' threw ' + e.constructor.name;
  }
  poke(o);
  return s + ' ' + show(o.b) + ' ' + show(o.f) + ' ' + show(o.h) + ' ' + show(o.e);
}
// A try: the anchor inside it, a throwing callee, members in the handler
// and after it.
function guarded(o, throwIt) {
  let s = '';
  try {
    s += show(o.a);
    poke(o);
    s += ' t' + show(o.c);
    if (throwIt) throw new Error('x');
    poke(o);
    s += ' t' + show(o.g);
  } catch (e) {
    s += ' c' + show(o.f) + show(o.b);
  }
  poke(o);
  return s + ' after' + show(o.h) + show(o.g);
}
// The anchor before the try: its fact reaches the handler too.
function guarded2(o, throwIt) {
  let s = show(o.e);
  try {
    poke(o);
    s += ' t' + show(o.a);
    if (throwIt) { poke(o); throw new Error('y'); }
    poke(o);
    s += ' t' + show(o.g);
  } catch (e) {
    s += ' c' + show(o.f) + show(o.b);
    poke(o);
    s += ' c' + show(o.d);
  }
  poke(o);
  return s + ' after' + show(o.h) + show(o.g);
}
// A getter between two covered reads does the mutating.
function viaGetter(o) {
  let s = show(o.f);
  s += o.trigger;
  return s + ' ' + show(o.g) + ' ' + show(o.f) + ' ' + show(o.a);
}

let warm = 0;
for (let k = 0; k < 4000; k++) {
  const o = k & 1 ? new N(k) : new M(k);
  warm += straight(o).length + blocks(o, k & 2).length + stores(o, k).length + guarded(o, k % 3 === 0).length;
  warm += guarded2(o, k % 5 === 0).length;
  warm += viaGetter(o).length;
}
console.log('warm', warm);
for (deep = false; ; deep = true) {
  for (mode = 1; mode <= 12; mode++) {
    for (const make of [(k) => new M(k), (k) => new N(k)]) {
      const r = [];
      r.push(straight(make(1)));
      r.push(blocks(make(2), true));
      r.push(blocks(make(3), false));
      r.push(stores(make(4), 4));
      r.push(guarded(make(5), false));
      r.push(guarded(make(6), true));
      r.push(guarded2(make(8), false));
      r.push(guarded2(make(9), true));
      if (!deep) r.push(viaGetter(make(7)));
      console.log((deep ? 'deep ' : '') + 'mode ' + mode + ': ' + r.join(' | '));
    }
  }
  if (deep) break;
}
console.log('log', log.join(','));
// After all of that, the facts hold again for fresh objects.
mode = 0;
let after = 0;
for (let k = 0; k < 1000; k++) after += straight(new M(k)).length + stores(new N(k), k).length;
console.log('after', after);
// The covered stores themselves, with the facts holding: each must land.
console.log(stores(new M(10), 10), '/', stores(new N(11), 11));
mode = 6;
console.log(stores(new M(12), 12), '/', stores(new N(13), 13));
