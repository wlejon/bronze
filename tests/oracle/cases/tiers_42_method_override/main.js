// A call the lowering devirtualizes to the nearest declaration above the
// receiver's inferred class must still reach an override, whether the
// override lives in another module or this one, and whether the site has
// already run hot on base receivers. A loop around such a call must not keep
// a property read hoisted past it on the strength of the base's body alone.
import { Layer, Counter } from './base.js';

let subHits = 0;
class Sub extends Layer {
  batchAt(a, b) {
    subHits++;
    return { sub: true, a, b };
  }
}

const sub = new Sub();
const first = sub.batch(9);
console.log('cold override', first.sub === true, first.a, first.b, subHits);

const base = new Layer();
let baseN = 0;
for (let i = 0; i < 200000; i++) baseN += base.batch(i & 63).n;
console.log('base warm', baseN, base.calls);

let mixed = 0;
for (let i = 0; i < 200000; i++) {
  const r = (i & 1 ? sub : base).batch(i & 15);
  mixed += r.sub ? 1 : 0;
}
console.log('mixed', mixed, subHits);

class Local {
  constructor() { this.v = 0; }
  step() { return this.inc(1); }
  inc(d) { this.v += d; return this.v; }
}
class LocalSub extends Local {
  inc(d) { this.v -= d; return this.v; }
}
const l = new Local();
for (let i = 0; i < 100000; i++) l.step();
const ls = new LocalSub();
for (let i = 0; i < 100000; i++) ls.step();
console.log('same module', l.v, ls.v);

class Bump extends Counter {
  tick(o) { o.x = o.x + this.k; return 1; }
}
console.log('hoist', new Counter().run(5), new Bump().run(5));
for (let i = 0; i < 20000; i++) new Counter().run(3);
console.log('hoist warm', new Bump().run(4));
