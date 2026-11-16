// A method reading and writing many fields of `this` on two receiver shapes
// that share a prefix (a class and a subclass of a subclass), so the first
// access tests the receiver's shape against the set of the two and leaves on
// a miss, and the rest fold. Then a receiver the set does not know: a second
// class named Leaf (its shape has the first one's name, so the harvest cannot
// tell them apart and the set's test fails on it, every time).
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
for (let i = 0; i < 30; i++) warm.push(i % 2 === 0 ? new Base(i) : new Leaf(i));
let total = 0;
for (let r = 0; r < 300; r++) total += run(warm, 3);
console.log('warm', total);

const odd = [new Twin(3), new Twin(9)];
const mixed = warm.slice(0, 6).concat(odd);
for (let r = 0; r < 12; r++) console.log('mixed', r, run(mixed, 1 + (r & 3)));
console.log(odd.map(o => o.out).join());
