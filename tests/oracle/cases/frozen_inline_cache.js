// A frozen object and a WARM inline cache. An entry says "this shape, this
// slot" and generated code turns a hit into an indexed load (docs/0010
// decision 7); `inline_cache_shape_changes.js` pins the two things an entry
// could already be wrong about, and non-writable is the third
// (docs/0021 decision 5).
//
// The answer is that `writable`, `configurable` and `extensible` live in the
// DICTIONARY and nowhere else, so freezing an object gives it a private shape
// that no cache entry has ever named — the shape transition key does not
// grow, and the cache layout in bronze_abi.h is untouched. This case is the
// proof: every site is warmed with a plain object FIRST, so its entry is
// filled, and the frozen receiver then arrives at a live entry.
//
// From ECMA-262 10.1.9.2 ([[Set]] on a non-writable data property is a silent
// false outside strict mode), 10.1.6.3 ([[DefineOwnProperty]] on a
// non-extensible object), 10.1.10.1 ([[Delete]] of a non-configurable
// property) and 20.1.2.7 (Object.freeze returns its argument):
//
// 1. A write through a warm site to a frozen receiver is DISCARDED, and the
//    read that follows it in the same function still sees the old value.
// 2. Freezing an object AFTER the site was warmed with that very object is
//    the harder half: the entry was filled from the object's own shape, and
//    the freeze has to invalidate it.
// 3. A frozen object takes no new properties, `delete` on it answers `false`
//    (which docs/0019 said bronze could never answer), and the element form
//    `o["k"] = v` is discarded exactly as the named form is.

function readV(p) {
  return p.v;
}
function writeV(p, x) {
  p.v = x;
  return p.v;
}

const a = { v: "a0" };
const b = { v: "b0" };
const frozen = { v: "f0" };

console.log(readV(a), readV(b));
console.log(writeV(a, "a1"), writeV(b, "b1"));

Object.freeze(frozen);
console.log(Object.isFrozen(frozen), Object.isFrozen(a));

const all = [a, frozen, frozen, b, frozen, a];
let out = "";
for (const p of all) {
  out = out + writeV(p, "w") + ";";
}
console.log(out);
console.log(readV(a), readV(b), readV(frozen));

frozen.fresh = 1;
console.log(frozen.fresh, Object.keys(frozen).join(","));
console.log(delete frozen.v, frozen.v);

const late = { v: "l0", pad: 1 };
for (let i = 0; i < 3; i = i + 1) writeV(late, "l" + i);
console.log(readV(late));
Object.freeze(late);
for (let i = 0; i < 3; i = i + 1) writeV(late, "x" + i);
console.log(readV(late), late.pad);

const byKey = Object.freeze({ k: "z" });
console.log(byKey["k"]);
byKey["k"] = "q";
console.log(byKey["k"]);
