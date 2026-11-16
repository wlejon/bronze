// Getter and method calls on one receiver shape at several sites of several
// functions (which share their member cell). Partway: a receiver of a subclass
// that overrides one member, an own property shadowing the getter, and the
// prototype's getter redefined.
class P {
  constructor(v) { this.v = v; }
  get dbl() { return this.v * 2; }
  inc(k) { return this.v + k; }
}
class Q extends P { get dbl() { return -this.v; } }
function f(o) { return o.dbl + o.inc(1); }
function g(o) { return o.dbl * 10 + o.inc(2) + o.dbl; }
function run(list) {
  let s = 0;
  for (const o of list) s += f(o) + g(o);
  return s;
}
const ps = [];
for (let i = 0; i < 16; i++) ps.push(new P(i));
let t = 0;
for (let r = 0; r < 4000; r++) t += run(ps);
console.log('warm', t);
console.log(run([ps[1], new Q(3), ps[2]]));
const shadow = new P(4);
Object.defineProperty(shadow, 'dbl', { value: 100 });
console.log(run([ps[3], shadow, ps[4]]));
Object.defineProperty(P.prototype, 'dbl', { get() { return this.v + 0.5; }, configurable: true });
console.log(run(ps.slice(0, 5)));
P.prototype.inc = function (k) { return k * 1000; };
console.log(run(ps.slice(0, 3)), f(new Q(2)), g(shadow));
