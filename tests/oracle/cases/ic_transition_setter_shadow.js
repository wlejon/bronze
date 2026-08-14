// The shape-transition write cache: a constructor body's `this.x = x` takes a
// recorded transition on every `new` after the first — until an INHERITED
// SETTER for that key appears, after which ECMA-262 10.1.9.2 says the
// assignment runs the setter and creates nothing. The cached transition must
// miss the moment the setter is defined (the prototype-mutation epoch), or
// instances would keep growing an own `x` the language says they no longer
// get. The hot loop first, so the cache is warm when the shadow lands.
function Pt(x) { this.x = x; }
let sum = 0;
for (let i = 0; i < 100; i = i + 1) {
  const p = new Pt(i);
  sum = sum + p.x;
}
console.log(sum);

const observed = [];
Object.defineProperty(Pt.prototype, "x", {
  get() { return "from-getter"; },
  set(v) { observed.push(v); },
});

const q = new Pt(7);
console.log(observed.length);
console.log(observed[0]);
console.log(q.x);
console.log(Object.keys(q).length);

const r = new Pt(8);
console.log(observed.length);
console.log(observed[1]);
