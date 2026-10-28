// A class constructor's adds take the inline shape transition, and the site's
// speculation guards only the helper's path behind it. Every construction
// after warm-up must stay on the transition without a deopt, and every way
// the transition can fail (a setter appearing on the prototype, a subclass's
// receiver, a frozen receiver, an overflow slot without room) must still
// reach the helper and do what the language says, armed or not.
class P {
  constructor(a, b, freeze) {
    if (freeze) Object.freeze(this);
    this.a = a;
    this.b = b;
    this.c = a + b;
    this.d = 0; this.e = 1; this.f = 2; this.g = 3; this.h = 4; this.i = 5; this.j = 6;
  }
}
class Q extends P {
  constructor(a) { super(a, a * 2, false); this.k = a; }
}

function sum(o) { return o.a + o.b + o.c + o.d + o.j; }

let total = 0;
for (let n = 0; n < 20000; n++) total += sum(new P(n, 1, false));
console.log('warm', total);

let sub = 0;
for (let n = 0; n < 5000; n++) { const q = new Q(n); sub += q.k + q.c; }
console.log('sub', sub, Object.keys(new Q(3)).join(','));

let threw = 0;
for (let n = 0; n < 50; n++) {
  try { new P(n, 2, true); } catch (e) { if (e instanceof TypeError) threw++; }
}
console.log('frozen threw', threw);

let seen = 0;
Object.defineProperty(P.prototype, 'e', { set(v) { seen += v; }, get() { return 'proto'; }, configurable: true });
let after = 0;
for (let n = 0; n < 20000; n++) { const p = new P(n, 3, false); after += p.c; if (p.e !== 'proto') after = -1; }
console.log('setter', seen, after, Object.prototype.hasOwnProperty.call(new P(1, 1, false), 'e'));

delete P.prototype.e;
let back = 0;
for (let n = 0; n < 20000; n++) back += new P(n, 4, false).e;
console.log('restored', back);
