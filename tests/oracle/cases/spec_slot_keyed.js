// Field reads and writes at sites whose receivers come in more shapes than a
// cache holds: every subclass runs Base's constructor first, so `a`, `b` and
// `c` sit at one slot in all of them and the speculated sites test the
// shape's entry for that slot. Receivers the test must not answer: an
// accessor installed over `b`, a frozen object, a dictionary-mode object, an
// object that is another's prototype (the write must reach it generically),
// a symbol-keyed shape, and a primitive `this`. `known` reaches the same
// sites inlined behind a test of one shape, which answers their slot tests.
"use strict";
class Base {
  constructor() { this.a = 1; this.b = 2; this.c = 3; }
  sum() { return this.a + this.b * 10 + this.c * 100; }
  bump(v) { this.b = v; }
}
class C1 extends Base { constructor() { super(); this.x1 = 1; } }
class C2 extends Base { constructor() { super(); this.x2 = 2; this.y2 = 2; } }
class C3 extends Base { constructor() { super(); this.x3 = 3; } }
class C4 extends C3 { constructor() { super(); this.x4 = 4; } }
class C5 extends Base { constructor() { super(); this[Symbol.iterator] = null; this.x5 = 5; } }
class C6 extends C2 { constructor() { super(); this.x6 = 6; } }
class C7 extends Base { constructor() { super(); this.x7 = 7; } }

function read(o) { return o.sum(); }
function write(o, v) { o.bump(v); }
function known(o, v) { const x = o.x7; o.bump(v + x); return o.sum(); }

function main() {
  const kinds = [C1, C2, C3, C4, C5, C6, C7, Base];
  const objs = [];
  for (let i = 0; i < 64; i++) objs.push(new kinds[i % kinds.length]());

  const accessor = new C1();
  let gets = 0, sets = 0;
  Object.defineProperty(accessor, "b", { get() { gets++; return 7; }, set(v) { sets++; }, configurable: true });
  const frozen = Object.freeze(new C2());
  const dict = new C3();
  delete dict.x3;
  dict.b = 9;
  const proto = new C7();
  const child = Object.create(proto);
  const odd = [accessor, frozen, dict, proto, child];
  const sevens = [new C7(), new C7(), new C7()];

  let total = 0, thrown = 0;
  for (let round = 0; round < 4000; round++) {
    for (let i = 0; i < objs.length; i++) {
      write(objs[i], round & 7);
      total += read(objs[i]);
    }
    for (const o of sevens) total += known(o, round & 3);
    if (round % 50 === 0) {
      for (const o of odd) {
        try { write(o, round & 3); } catch (e) { thrown++; }
        total += read(o);
      }
      try { total += Base.prototype.sum.call(5) | 0; } catch (e) { thrown++; }
    }
  }
  console.log(total, gets, sets, thrown, frozen.b, dict.b, proto.b, child.b,
              Object.keys(child).length, Object.getOwnPropertyNames(dict).join(","));
}
main();
