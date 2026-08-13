// A NAMED property on an array — `a.foo = 1`, which is ordinary JavaScript
// because an array is an object, and which every operation defined over own
// keys then has to agree about.
//
// An array's own properties come from three different places (ECMA-262 10.4.2):
// the ELEMENTS, the exotic `length`, and — for every other name — an ordinary
// property. The third is what this case pins, and the reason it needs pinning
// is that each of the six paths that can see one reaches an array from its own
// direction: a read, a write, `in`, `delete`, `for-in` and the own-key walks.
//
// What it pins, from 6.1.7.1 (OwnPropertyKeys order), 7.3.23
// (EnumerableOwnProperties), 7.3.25 (CopyDataProperties), 13.5.1 (delete),
// 14.7.5.6 (for-in), 23.1.3.41 (the array iterator) and 25.5.2 (JSON):
//
// 1. A named write does not move `length` and an element write does not
//    disturb a named property. They are separate storage and the language
//    treats them so.
// 2. Own-key ORDER is integer-like keys ascending and then the rest in
//    INSERTION order — so a named property always comes after every index, and
//    a name deleted and re-added goes to the END, because the delete took its
//    position with it. `Object.keys`, `Object.values`, `Object.entries`,
//    `for-in` and object spread all report that one order.
// 3. `[...a]` and `JSON.stringify(a)` do NOT see named properties, and neither
//    is a special case: array spread is the ITERATOR, which walks
//    `0..length-1`, and 25.5.2.5 SerializeJSONArray is defined the same way.
//    `{ ...a }` DOES see them, because object spread is defined over own keys.
// 4. An own property SHADOWS the `Array.prototype` method of the same name —
//    `a.map = 5` reads 5, `typeof a.map` is "number" — and `delete` brings the
//    method back, because it removes an own property and never touches the
//    prototype. A named property whose value is `undefined` shadows just as
//    hard: reading it is undefined, and `in` and `hasOwnProperty` are still
//    true, which is the difference between an absent property and a present
//    one holding nothing.
// 5. `a.k` and `a["k"]` are one property (7.1.19 ToPropertyKey), and a key that
//    is a number but NOT a canonical array index — `a[1.5]` — names a property
//    rather than an element.
// 6. `hasOwnProperty` and `in` answer different questions about a method name:
//    `"map" in a` is true because the prototype has it, `a.hasOwnProperty("map")`
//    is false until something writes one.
// 7. The array methods are untouched by a named property: it is not an element,
//    so it is not sorted, searched, iterated or copied by `concat`/`slice`.
const a = [10, 20];
a.tag = "t";
a.count = 2;
console.log(a.tag);
console.log(a.count);
console.log(a.length);
console.log(a[0], a[1]);
console.log("tag" in a, "nope" in a, "length" in a, "map" in a);
console.log(a.hasOwnProperty("tag"), a.hasOwnProperty("0"), a.hasOwnProperty("map"));
console.log(a.propertyIsEnumerable("tag"));
console.log(Object.keys(a).join(","));
console.log(Object.values(a).join(","));
console.log(JSON.stringify(Object.entries(a)));
let ks = "";
for (const k in a) {
  ks = ks + k + "|";
}
console.log(ks);
console.log(JSON.stringify(a));
console.log(JSON.stringify([...a]));
console.log(JSON.stringify({ ...a }));
console.log(JSON.stringify(Object.assign({}, a)));
console.log(a);

// An element write leaves the named properties alone, and vice versa.
a[2] = 30;
console.log(a.length, a.tag, Object.keys(a).join(","));

// delete removes it; re-adding puts it at the END of the insertion order.
console.log(delete a.tag);
console.log("tag" in a, a.tag, a.hasOwnProperty("tag"));
a.tag = "again";
console.log(Object.keys(a).join(","));
console.log(a.tag);

// Shadowing a prototype method, and un-shadowing it.
const s = [1, 2, 3];
console.log(typeof s.map);
s.map = 5;
console.log(s.map, typeof s.map, s.hasOwnProperty("map"));
console.log(Object.keys(s).join(","));
console.log(delete s.map);
console.log(typeof s.map);
console.log(s.map((v) => v * 2).join(","));

// A named property holding `undefined` is present, not absent.
s.join = undefined;
console.log(s.join, "join" in s, s.hasOwnProperty("join"));
console.log(delete s.join);
console.log(typeof s.join);

// One property, two spellings — and a numeric key that is not an index.
const c = [7];
c["k"] = 1;
console.log(c.k);
c.k2 = 2;
console.log(c["k2"]);
c[1.5] = "frac";
console.log(c["1.5"], c[1.5], c.length);
console.log(Object.keys(c).join(","));
console.log(delete c[1.5], c["1.5"], Object.keys(c).join(","));

// The array methods do not see it.
const m = [3, 1, 2];
m.note = "hi";
console.log(m.sort().join(","));
console.log(m.length, m.note);
console.log(m.indexOf("hi"), m.includes("hi"));
console.log(m.concat([4]).length, m.concat([4]).note);
console.log(m.slice(0, 2).note);
console.log(m.map((v) => v).note);
let seen = "";
m.forEach((v) => {
  seen = seen + v;
});
console.log(seen);
let visited = "";
for (const v of m) {
  visited = visited + v;
}
console.log(visited);

// A property deleted from an array still leaves the elements alone.
const h = [1];
h.p = 1;
console.log(delete h.p);
h[1] = 2;
h.q = 3;
console.log(JSON.stringify(h), h.q, Object.keys(h).join(","));
