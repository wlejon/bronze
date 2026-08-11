// One property expression, several receiver LAYOUTS — the case docs/0019
// decision 5 exists for. An inline cache entry says "this shape, this slot"
// and generated code turns a hit into an indexed load with no call
// (docs/0010 decision 7). Both features of this chunk break that assumption
// if the entry is allowed to describe them:
//
//  - a DICTIONARY object's slots are not shape-indexed and its slot numbers
//    are reused after a delete, so a stale entry reads an unrelated property;
//  - an ACCESSOR is not a slot at all, so a hit that loaded it as data would
//    return the getter function rather than call it.
//
// Every site below is warmed with a PLAIN object first, deliberately: a site
// whose first receiver is a dictionary never fills its entry, so it would
// pass without proving anything. The order is plain, plain, then the awkward
// shapes, then plain again — the entry must be filled, then missed, then hit
// again.
//
// Expectations are ECMA-262 6.1.7.2 (Get / OrdinaryGet), 10.5.6
// ([[Delete]]) and 10.1.8.1 (a getter runs with `this` as the receiver).
// Nothing here depends on inference, so the two runs must agree byte for
// byte; a divergence between them is the inline form disagreeing with the
// helper, which is exactly the bug this case is written against.

// ---- one site, three receiver layouts -------------------------------------
function read(p) {
  return p.v;
}

const plainA = { v: "plainA" };
const plainB = { v: "plainB" };

// A dictionary: the delete cannot unlink a shape node, so this object stops
// being a record. `v` survives the delete and keeps its slot.
const dict = { pad: 1, v: "dict" };
delete dict.pad;

// An accessor: `v` is a pair of functions, and reading it CALLS one, which
// is observable because the getter counts its own calls.
const acc = {
  n: 0,
  get v() {
    this.n = this.n + 1;
    return "acc#" + this.n;
  },
};

console.log(read(plainA));
console.log(read(plainB));

// The awkward shapes appear CONSECUTIVELY as well as alternating. An entry
// wrongly filled with an accessor or a dictionary only does damage on the
// NEXT read of the same shape, so a list that never repeats one would leave
// the poisoned entry sitting there unused.
const all = [plainA, plainB, dict, dict, acc, acc, plainA, dict, acc, plainB];
for (const p of all) {
  console.log(read(p));
}
console.log(acc.n);

// A dictionary's shape is minted once and does NOT change on the deletes
// after the first — which is why an entry naming one is refused rather than
// merely refreshed. Here the same object frees a slot and hands it to a new
// name while its shape word stays exactly what it was.
const churn = { k: "k1", v: "churn" };
delete churn.k;
console.log(read(churn));
console.log(read(churn));
console.log(delete churn.v);
churn.other = "other";
console.log(read(churn));
churn.v = "churn2";
console.log(read(churn));
console.log(Object.keys(churn).join(","));

// ---- the same, at a site inference proves monomorphic ----------------------
// `o` has one known shape class, so this read is emitted as the INLINED
// cache check rather than a helper call (docs/0010 decision 7). The delete
// happens halfway through the loop, so the same inlined check sees the
// transition shape twice and the private dictionary shape twice.
const o = { pad: 1, v: "before" };
for (let i = 0; i < 4; i = i + 1) {
  if (i === 2) {
    console.log(delete o.pad);
  }
  console.log(o.v);
}
o.v = "after";
console.log(o.v);
console.log(Object.keys(o).join(","));

// An accessor at a site inference also proves monomorphic: the shape word
// matches the class it proved, and the entry is still empty, because an
// accessor is never written into one.
const g = { s: 1, get v() { return this.s * 10; } };
for (let i = 0; i < 3; i = i + 1) {
  console.log(g.v);
  g.s = g.s + 1;
}
