// Map and Set: ECMA-262 24.1 and 24.2, an insertion-ordered table whose KEY is
// an arbitrary value rather than a property name.
//
// 1. Keys are compared by SameValueZero (7.2.10), not by `===` and not by
// string coercion. `1` and `"1"` are two different keys, where an object used
// as a map would collapse them; NaN is found by NaN, where `===` never would;
// and `-0` and `0` are the same key. 2. An object key is compared by IDENTITY,
// so two structurally identical literals are two keys — the thing a
// string-keyed object cannot express at all. The table hashes such a key by its
// ADDRESS, so this half of the case is also what the index epoch exists for:
// run it under oracle-gc-stress and every one of these lookups happens on a
// table the collector has moved out from under. 3. Insertion order is the
// iteration order, and `set` on an EXISTING key updates the value in place
// without moving it. Deleting and re-adding does move it to the end. 4. `size`
// is a live accessor, and `delete` returns whether something was removed,
// unlike `Object`'s. `delete` also spells the ordinary property name it is: a
// reserved word after a `.` is an IdentifierName (12.7.1). 5. A Set is the same
// table without values: `add` of a present element is a no-op that keeps the
// original position, and NaN deduplicates against itself. 6. Both are iterable,
// and `new Set(iterable)` consumes one — which is the iterator protocol rather
// than the index walk `for-of` used to lower to, since a Map
// iterator has no `length` to walk.
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
