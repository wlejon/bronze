// BLOCKED: `Map` and `Set` are `undefined variable: Map` / `undefined
// variable: Set`. docs/0011 decision 1 says a global bronze does not provide
// is a named error at the point of use rather than a stub, so this is the
// diagnosis working — but it is the last big gap between an object used as a
// record and a real keyed collection.
//
// The blocker is that these are the first builtins whose KEY is an arbitrary
// value rather than a string. Every keyed structure bronze has — shapes,
// inline caches, `Object.keys`, `for-in` — is built on interned string names
// (docs/0009), and SameValueZero over NaN-boxed values needs a hash the GC
// can survive: a moving collector (docs/0004 decision 4) invalidates an
// object-identity hash on every collection, so the table has to be rehashed
// at collection time or keys have to carry a stable id. That is a runtime
// data-structure decision, not a syntax one, which is why it sits behind
// `delete`'s dictionary mode: the same side table serves both.
//
// There is a second blocker, and this case pins it deliberately. docs/0012
// decision 3 lowers `for-of` as an INDEX WALK over a length, which covers
// arrays and strings and nothing else. A Map is the first value whose
// iteration is a real iterator — `order.keys()` returns an iterator object
// with no `length` at all, and `[...fromArr]` spreads one. So landing Map and
// Set also means landing the iterator protocol (`Symbol.iterator`, `next()`,
// the done/value pair) and making the index walk the fast path rather than
// the only path.
//
// What this case pins when it lands, from ECMA-262 24.1 (Map) and 24.2 (Set):
//
// 1. Map keys are compared by SameValueZero, not by `===` and not by string
//    coercion. `1` and `"1"` are two different keys, where an object used as
//    a map would collapse them; NaN is found by NaN, where `===` never
//    would; and `-0` and `0` are the same key.
// 2. An object key is compared by IDENTITY, so two structurally identical
//    literals are two keys — the thing a string-keyed object cannot express
//    at all.
// 3. Insertion order is the iteration order, and `set` on an EXISTING key
//    updates the value in place without moving it. Deleting and re-adding
//    does move it to the end.
// 4. `size` is a live accessor, and `delete` returns whether something was
//    removed, unlike `Object`'s.
// 5. A Set is the same table without values: `add` of a present element is a
//    no-op that keeps the original position, and NaN deduplicates against
//    itself.
const m = new Map();
m.set(1, "num");
m.set("1", "str");
m.set(NaN, "nan");
m.set(-0, "zero");
console.log(m.size);
console.log(m.get(1));
console.log(m.get("1"));
console.log(m.get(NaN));
console.log(m.get(0));
console.log(m.has(2));
console.log(m.get(2));

const ka = { id: 1 };
const kb = { id: 1 };
m.set(ka, "a");
m.set(kb, "b");
console.log(m.get(ka));
console.log(m.get(kb));
console.log(m.size);

const order = new Map();
order.set("x", 1);
order.set("y", 2);
order.set("z", 3);
order.set("x", 10);
let keys = "";
for (const k of order.keys()) {
  keys = keys + k;
}
console.log(keys);
console.log(order.get("x"));
console.log(order.delete("y"));
console.log(order.delete("y"));
console.log(order.size);
order.set("y", 20);
let keys2 = "";
for (const k of order.keys()) {
  keys2 = keys2 + k;
}
console.log(keys2);

let pairs = "";
for (const e of order) {
  pairs = pairs + e[0] + "=" + e[1] + ";";
}
console.log(pairs);

const s = new Set();
s.add(1);
s.add(1);
s.add("1");
s.add(NaN);
s.add(NaN);
console.log(s.size);
console.log(s.has(1));
console.log(s.has(NaN));
console.log(s.has(2));
let items = "";
for (const v of s) {
  items = items + typeof v + ",";
}
console.log(items);
const fromArr = new Set([3, 1, 3, 2]);
console.log(fromArr.size);
console.log([...fromArr].join(","));
