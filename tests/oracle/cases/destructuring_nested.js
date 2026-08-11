// Patterns that contain patterns, and patterns in a for-of head (docs/0017
// decisions 4 and 6).
//
// A nested pattern is destructured from the value the enclosing element
// produced, defaults included — so in `{ q: { w = 1 } = {} }` the missing
// `q` first becomes `{}` and only then is read for `w`, which is what makes
// the answer 1 rather than a failure. `{ a: { b: [c, { d = 4 }] } }` mixes
// all three at once: rename, index, and a default on a missing property.
//
// The for-of head is the same pattern machinery, per iteration: array
// patterns over pairs and object patterns over records, with a default that
// fires only for the record that omits the key.
//
// The object rest at the end also pins enumeration order — `{ b: 2, c: 3 }`
// keeps insertion order (docs/0009 decision 1), and `a` is excluded because
// the pattern named it.
const { a: { b: [c, { d = 4 }] } } = { a: { b: [3, {}] } };
console.log(c + ":" + d);
const [[x1, y1], [x2, y2]] = [[1, 2], [3, 4]];
console.log(x1 + y1 + x2 + y2);
const { q: { w = 1 } = {} } = {};
console.log(w);
const { list: [head, ...tail] } = { list: [1, 2, 3] };
console.log(head + ":" + tail.length);
function pick({ p: [q, r = 9] }) { return q + ":" + r; }
console.log(pick({ p: [1] }));
console.log(pick({ p: [1, 2] }));
for (const [k, v] of [[1, "one"], [2, "two"]]) {
  console.log(k + "=" + v);
}
for (const { name, n = 0 } of [{ name: "a", n: 5 }, { name: "b" }]) {
  console.log(name + n);
}
const { a: aa, ...others } = { a: 1, b: 2, c: 3 };
console.log(aa);
console.log(others);
