// spec_shape_set's method meeting receivers of every kind the shape set does
// not hold: a second class named Leaf, a fourth subclass, an instance whose
// shape changed after construction, one whose field became an accessor, a
// dictionary-mode one, and a call in the middle of the method that gives
// `this` a new shape between two of its reads.
class Base {
  constructor(k) { this.tick = 0; this.seen = -1; this.x = k; this.y = k * 2; this.sx = 1; this.sy = 1; this.out = 0; }
  update(t) {
    const tick = this.tick;
    if (this.seen === tick) return 0;
    this.seen = tick;
    const a = this.x * this.sx;
    const b = this.y * this.sy;
    this.out = a + b + this.hook(t);
    return this.out + this.x - this.y;
  }
  hook(t) { return t & 1; }
}
class Mid extends Base { constructor(k) { super(k); this.m = 3; } }
class Leaf extends Mid { constructor(k) { super(k); this.l = 4; } }
const Twin = (() => class Leaf extends Mid { constructor(k) { super(k); this.l = 8; } })();
class Fourth extends Leaf { constructor(k) { super(k); this.f = 5; } }
class Grows extends Base {
  hook(t) { if (t === 2) this['late' + t] = t; return 2; }
}
function run(list, rounds) {
  let s = 0;
  for (let r = 0; r < rounds; r++) {
    for (const o of list) {
      o.tick = r;
      s += o.update(r);
    }
  }
  return s;
}
const warm = [];
for (let i = 0; i < 30; i++) warm.push(i % 3 === 0 ? new Base(i) : i % 3 === 1 ? new Mid(i) : new Leaf(i));
let total = 0;
for (let r = 0; r < 300; r++) total += run(warm, 3);
console.log('warm', total);

const late = new Base(5);
late.extra = 1;
const acc = new Mid(6);
let backing = 9;
Object.defineProperty(acc, 'sx', { get() { return backing; }, set(v) { backing = v + 1; } });
const dict = new Leaf(7);
for (let i = 0; i < 40; i++) dict['k' + i] = i;
for (let i = 0; i < 40; i++) delete dict['k' + i];
const grows = new Grows(8);
const odd = [new Twin(3), new Fourth(4), late, acc, dict, grows];
const mixed = warm.slice(0, 6).concat(odd);
for (let r = 0; r < 12; r++) console.log('mixed', r, run(mixed, 1 + (r & 3)));
console.log(odd.map(o => o.out).join(), backing, Object.keys(grows).join());
