// A counter bumped through a receiver of a few shapes (a polymorphic read and
// write chain), then reached through receivers of shapes none of the chain's
// tests know: a fourth class, a subclass, a dictionary-mode object, one with
// the key on an accessor of its prototype, and a frozen one.
class A { constructor() { this.n = 0; this.a = 1; } }
class B { constructor() { this.b = 1; this.n = 0; } }
class C { constructor() { this.c = 1; this.d = 2; this.n = 0; } }
class D { constructor() { this.e = 1; this.f = 2; this.g = 3; this.n = 0; } }
class E extends C { constructor() { super(); this.h = 9; } }
class P { get n() { return this._n | 0; } set n(v) { this._n = v + 100; } }
function bump(list, times) {
  let s = 0;
  for (let t = 0; t < times; t++) {
    for (let i = 0; i < list.length; i++) {
      const o = list[i];
      o.n++;
      s += o.n;
    }
  }
  return s;
}
const base = [new A(), new B(), new C(), new A(), new B(), new C()];
let total = 0;
for (let r = 0; r < 2000; r++) total += bump(base, 2);
console.log('warm', total, base.map(o => o.n).join());
const dict = new A();
for (let i = 0; i < 40; i++) dict['k' + i] = i;
for (let i = 0; i < 40; i++) delete dict['k' + i];
const frozen = Object.freeze(new B());
const odd = [new D(), new E(), dict, new P(), frozen, {}, Object.create(null)];
const mixed = base.concat(odd);
for (let r = 0; r < 3; r++) console.log('mixed', r, bump(mixed, r + 1));
console.log(mixed.map(o => String(o.n)).join(), odd[3]._n, Object.keys(odd[5]).join());
