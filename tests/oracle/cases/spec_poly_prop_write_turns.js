// A property write whose receivers held two or three shapes (a polymorphic
// store chain), then partway through a run a receiver of a shape none of the
// tests know: one with the key on a setter of its prototype, a frozen one,
// one in dictionary mode, and one where the key is missing and gets added.
class A { constructor() { this.x = 0; this.a = 1; } }
class B { constructor() { this.b = 2; this.x = 0; } }
class C { constructor() { this.c = 3; this.d = 4; this.x = 0; } }
class S { get x() { return this._x | 0; } set x(v) { this._x = v * 10; } }
function store(list, v) {
  let sum = 0;
  for (let i = 0; i < list.length; i++) {
    const o = list[i];
    o.x = v + i;
    sum += o.x;
  }
  return sum;
}
const base = [];
for (let i = 0; i < 30; i++) base.push(i % 3 === 0 ? new A() : i % 3 === 1 ? new B() : new C());
let total = 0;
for (let r = 0; r < 3000; r++) total += store(base, r);
console.log('warm', total);
const frozen = Object.freeze(new A());
const dict = new B();
for (let i = 0; i < 40; i++) dict['k' + i] = i;
for (let i = 0; i < 40; i++) delete dict['k' + i];
const odd = [new A(), new S(), new B(), frozen, new C(), dict, {}, new A()];
const mixed = base.slice(0, 10).concat(odd, base.slice(10, 14));
for (let r = 0; r < 4; r++) console.log('mixed', r, store(mixed, r * 7));
console.log(frozen.x, odd[1].x, odd[1]._x, dict.x, odd[6].x, Object.keys(odd[6]).join());
console.log(base.map(o => o.x).join(','));
