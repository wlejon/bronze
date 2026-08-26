// Stage R2's generated code, from a program's side.
//
// R1 gave a slot a representation and made the RUNTIME honour it. R2 spends
// that at the store site: where the compiler can prove the value it is about to
// store is a Number, it stores the eight bytes with no test at all -- into a
// double slot and into a boxed slot alike, because a Number Value's bits ARE
// the canonical double the double slot promises. Every line below is a way for
// that identity to be false, and every one of them prints the same bytes under
// BRONZE_SLOT_REPR_OBSERVED=1 (every eligible field born a double slot), under
// BRONZE_NO_SLOT_REPR=1 (none of them ever), under the default, and with
// BRONZE_NO_REPR_CODEGEN=1 compiled in (the R1 sequence emitted instead).
//
// Deliberately printed as booleans and exact literals: an expectation derived
// by hand from ECMA-262 is only worth having if it can be read off the spec,
// and a sum of doubles cannot.

class Cell {
  constructor(p, q, r) {
    this.p = p;
    this.q = q;
    this.r = r;
  }
}

// 1. THE NaN FRONTIER. A NaN a raw store puts in a slot has to come back as the
//    NaN a boxed store would have: bronze NaN-boxes, so a non-canonical NaN's
//    bits alias a TAG and the value would read back as an object, a string or
//    the hole. Every NaN below is COMPUTED (a literal `NaN` could be folded to
//    the canonical pattern at compile time and would prove nothing).
const zero = Number('0');
const inf = 1 / zero;
const c = new Cell(zero / zero, inf - inf, Math.sqrt(-1));
console.log('nan self-inequal:', c.p !== c.p, c.q !== c.q, c.r !== c.r);
console.log('nan isNaN:', Number.isNaN(c.p), Number.isNaN(c.q), Number.isNaN(c.r));
console.log('nan typeof:', typeof c.p, typeof c.q, typeof c.r);
console.log('nan Object.is:', Object.is(c.p, NaN), Object.is(c.p, c.q));
console.log('nan String:', String(c.p), String(c.q), String(c.r));
console.log('nan JSON:', JSON.stringify(c));
console.log('nan arith:', c.p + 1 !== c.p + 1, Math.max(c.p, 1) !== Math.max(c.p, 1));
// A NaN read out of one slot and stored straight into another: the round trip
// the raw store makes shortest.
const c2 = new Cell(1.5, 2.5, 3.5);
c2.p = c.p;
c2.q = c2.p;
c2.r = c2.q * 2;
console.log('nan relay:', c2.p !== c2.p, c2.q !== c2.q, c2.r !== c2.r);
console.log('nan relay typeof:', typeof c2.p, typeof c2.q, typeof c2.r);

// 2. THE SIGNED ZERO and the infinities, the other two values a representation
//    can lose on the way through.
const z = new Cell(-0, inf, -inf);
console.log('zero:', Object.is(z.p, -0), 1 / z.p, z.p === 0);
console.log('inf:', z.q, z.r, 1 / z.q, Object.is(z.r, -Infinity));
console.log('zero json:', JSON.stringify(z));

// 3. GENERALIZATION UNDER A GUARD. The same store site runs before and after
//    the slot stops being a double, and a shape-mate that was never written
//    must not notice either time. This is the interleaving a raw store has to
//    survive: the bytes it writes are correct for both representations, so the
//    generalization between two visits to the site changes nothing it did.
function fill(o, n) {
  o.p = n;
  o.q = n + 0.5;
  o.r = n + 0.25;
  return o.p + o.q + o.r;
}
const g = new Cell(0, 0, 0);
const mate = new Cell(9.5, 8.5, 7.5);
console.log('before:', fill(g, 1), g.p, g.q, g.r);
g.q = 'a string';
console.log('generalized:', g.p, g.q, g.r, typeof g.q);
console.log('after:', fill(g, 2), g.p, g.q, g.r, typeof g.q);
console.log('mate untouched:', mate.p, mate.q, mate.r);
// And the reverse order: generalize FIRST, so the site's very first run already
// faces a boxed slot.
const g2 = new Cell(0, 0, 0);
g2.r = null;
console.log('pre-generalized:', fill(g2, 3), g2.p, g2.q, g2.r);

// 4. INT32 STORES. `n | 0` is an Int32 in the IL, whose boxed bits are a tag
//    and a payload rather than an f64 -- the one value a raw store may NOT
//    write into a double slot unconverted.
class Counter {
  constructor() {
    this.n = 0;
    this.m = 0;
  }
}
const k = new Counter();
for (let i = 0; i < 5; i++) {
  k.n = i | 0;
  k.m = (i * 3) | 0;
}
console.log('int32:', k.n, k.m, typeof k.n, k.n === 4, 1 / (0 | 0));
k.n = -1 | 0;
console.log('int32 neg:', k.n, k.n < 0, k.n + 0.5);
k.n = 0.5;
console.log('int32 then double:', k.n, k.n === 0.5);

// 5. ACCESSORS, which a raw store must never reach: an accessor occupies two
//    slots and running one is a call, not a store.
class Temp {
  constructor(v) {
    this._v = v;
  }
  get v() {
    return this._v * 2;
  }
  set v(n) {
    this._v = n / 2;
  }
}
const t = new Temp(4.5);
t.v = 10;
console.log('accessor:', t._v, t.v, typeof t.v);
// A data slot turned into an accessor underneath a site that has been storing
// numbers into it.
const acc = new Cell(1.5, 2.5, 3.5);
fill(acc, 7);
let seen = 0;
Object.defineProperty(acc, 'p', {
  get() {
    return 42.5;
  },
  set(n) {
    seen = n;
  },
  configurable: true,
});
fill(acc, 8);
console.log('accessor swap:', acc.p, seen, acc.q, acc.r);

// 6. DICTIONARY MODE. Deleting a property drops an object out of the shape
//    world, and every store into it afterwards goes the slow way.
const dict = new Cell(1.5, 2.5, 3.5);
delete dict.q;
dict.a = 1.25;
dict.b = 2.25;
dict.q = 4.5;
console.log('dict:', dict.p, dict.q, dict.r, dict.a, dict.b);
console.log('dict keys:', Object.keys(dict).join(','));
dict.p = zero / zero;
console.log('dict nan:', dict.p !== dict.p, typeof dict.p);

// 7. ALL OF IT UNDER ALLOCATION. A slot the collector must not trace is one
//    holding a double whose bits look like a pointer; the subnormals below are
//    exactly that pattern, and the loop makes a collection run while they are
//    live and while a NaN sits beside them.
const alias = new Cell(5e-324, 1.5e-323, 2.5e-323);
const nanHolder = new Cell(zero / zero, 2.5, 3.5);
let live = 0;
for (let i = 0; i < 40000; i++) {
  const tmp = new Cell(i * 0.5, i * 0.25, i * 0.125);
  tmp.p = tmp.q + tmp.r;
  if (tmp.p > 0) live++;
  if ((i & 4095) === 0) {
    const junk = [];
    for (let j = 0; j < 100; j++) junk.push(new Cell(j, j + 0.5, zero / zero));
    if (junk[7].r !== junk[7].r) live++;
  }
}
console.log('live:', live);
console.log('alias survived:', alias.p === 5e-324, alias.q === 1.5e-323, alias.r === 2.5e-323);
console.log('nan survived:', nanHolder.p !== nanHolder.p, nanHolder.q, nanHolder.r);
console.log('alias typeof:', typeof alias.p, typeof alias.q, typeof alias.r);
