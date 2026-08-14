// The inline `new` fast path (llvm_construct.cpp): a vetted plain constructor
// is bump-allocated and called directly in generated code, with the helper
// kept for every slow case. This case pins the pieces a wrong guard or a
// missed root would silently break, and it is shaped for the suite's
// BRONZE_GC_STRESS re-run: the constructor body allocates (string concat, so
// under stress every construction collects MID-CONSTRUCT while the fresh
// instance is live only through generated code's root frame), and live
// references are held across every allocation — a growing prototype chain of
// older instances, interned-then-copied strings, and an array being grown.
function Pt(x, s) {
  this.x = x;
  this.tag = s + x;
  this.prev = null;
}
let head = null;
const keep = [];
let sum = 0;
for (let i = 0; i < 2000; i = i + 1) {
  const p = new Pt(i, "n");
  p.prev = head;
  head = p;
  if (i % 100 === 0) keep.push(p);
  sum = sum + p.x;
}
let n = 0;
let q = head;
while (q !== null) {
  n = n + 1;
  q = q.prev;
}
console.log(n);
console.log(sum);
console.log(keep.length);
console.log(keep[19].tag);
console.log(head.tag);

// A constructor returning an object replaces the instance (ECMA-262 10.2.2
// step 9-ish: [[Construct]] returns the body's object result); returning a
// primitive leaves the instance. Both forms must agree between the inline
// path and the helper.
function Boxed(v) {
  this.v = v;
  return { replaced: v * 2 };
}
console.log(new Boxed(21).replaced);
function Prim() {
  this.k = 1;
  return 5;
}
console.log(new Prim().k);

// A short call: argc < arity misses the inline guard and takes the helper,
// whose FunctionHeader::call pads with undefined — the two paths must give
// the same instance.
function Pad(a, b) {
  this.a = a;
  this.b = b;
}
const padded = new Pad(7);
console.log(padded.a);
console.log(padded.b === undefined);
