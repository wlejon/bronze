// A merged read run raw-unboxes its values at their definition, before their
// is.number test. A dynamic xor joins the run only if its producer is a read:
// `o.low ^ o.w2` can be a BigInt (or, here, undefined ^ 0 = 0), so it must be
// tested before it is unboxed, or the proof certificate is refused.
function mk() { return { w2: 0, w3: 0, length: 0 }; }
function h(o) { return o.low ^ o.w2 ^ o.w3 ^ o.length ^ o.b7; }
console.log(h(mk()));
function g(o) { return (o.a ^ o.b) + (o.c ^ o.d) + o.e; }
console.log(g({ a: 1, b: 2, c: 4, d: 8, e: 16 }));
console.log(g({ a: 1, b: 2, c: 4, d: 8, e: 0.5 }));
let big = 0;
function h2(o) { return o.low ^ o.w2 ^ o.w3 ^ o.length ^ o.b7; }
try { big = h2({ low: 1n, w2: 2n, w3: 3n, length: 4n, b7: 5n }); } catch (e) { big = e.constructor.name; }
console.log(String(big));
