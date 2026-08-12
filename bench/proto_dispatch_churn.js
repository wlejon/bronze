// The same depth-3 inherited read as proto_dispatch.js, interleaved with an
// object construction. Every `this.x = x` is a property ADD, and an add is what
// invalidates a depth > 0 inline-cache entry — so this benchmark exists to
// prove that an add to an ORDINARY object does not.
//
// It is the control that a coarser invalidation rule fails: counting every
// add rather than only the ones landing on a prototype measured 2739ms here
// against 1960ms, while leaving proto_dispatch.js untouched. A regression
// shows up in the gap between this file and churn-free construction, never in
// either number alone.
class A {}
A.prototype.k = 1;
class B extends A {}
class C extends B {}
function Pt(x) { this.x = x; }
const o = new C();
let sum = 0;
for (let i = 0; i < 3000000; i = i + 1) {
  const p = new Pt(i);
  sum = sum + o.k + p.x - p.x;
}
console.log(sum);
