// The `Object` namespace beyond `keys`. `Object.keys` used to be recognised at
// the CALL SITE and nothing else on `Object` existed; it is a real object now,
// and the fast path for `keys` calls the same function the member does — so the
// two can never answer differently.
//
// From ECMA-262 20.1.2.1 (assign), 20.1.2.5 (entries), 20.1.2.24 (values) and
// 20.1.2.7 (fromEntries), all of which are EnumerableOwnProperties over the the
// own-key order the shape chain pins:
//
// 1. `keys`, `values` and `entries` walk the same order, which is insertion
// order and not sorted — `{ b, a, c }` reports b, a, c. 2. All three see only
// ENUMERABLE own properties, so a `defineProperty` that omitted `enumerable` is
// invisible to every one of them, and to `assign`. 3. `assign` MUTATES its
// first argument and returns it, later sources win, and it copies values rather
// than descriptors. 4. `fromEntries` consumes an ITERABLE of pairs, so a Map
// goes through it directly and a repeated key keeps the last value. 5. An
// array's own keys are its indices as strings, in index order.

const src = { b: 2, a: 1, c: 3 };
console.log(Object.keys(src).join(","));
console.log(Object.values(src).join(","));
const flat = [];
for (const e of Object.entries(src)) flat.push(e[0] + "=" + e[1]);
console.log(flat.join(";"));

const target = { a: "old", z: 0 };
const merged = Object.assign(target, { a: "new", y: 9 }, { y: 10 });
console.log(merged === target, Object.keys(target).join(","), target.a, target.y);

const hidden = {};
Object.defineProperty(hidden, "skip", { value: 1, enumerable: false });
Object.defineProperty(hidden, "show", { value: 2, enumerable: true });
console.log(Object.keys(hidden).join(","), Object.values(hidden).join(","));
console.log(Object.keys(Object.assign({}, hidden)).join(","));

const built = Object.fromEntries([
  ["one", 1],
  ["two", 2],
  ["one", 3],
]);
console.log(Object.keys(built).join(","), built.one, built.two);

const m = new Map();
m.set("x", 1);
m.set("y", 2);
const fromMap = Object.fromEntries(m);
console.log(Object.keys(fromMap).join(","), fromMap.x, fromMap.y);
console.log(Object.entries(fromMap).length);

const arr = ["p", "q"];
console.log(Object.keys(arr).join(","), Object.values(arr).join(","));
