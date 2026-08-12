// `JSON.stringify` and `JSON.parse` (ECMA-262 25.5), and the pinned bytes of a
// format that is deliberately NOT console.log's.
//
// The two halves are two files and one module boundary: `src/json` turns code
// units into a tree and rejects everything JavaScript allows and JSON does not,
// and it can be driven with no heap at all (`tests/json`), while
// `src/runtime/json_stringify.cpp` owns the output bytes.
//
// What this case pins, from ECMA-262 25.5.2 (stringify) and 25.5.1 (parse):
//
// 1. Key order in the output is own-enumerable insertion order, not sorted.
// 2. `undefined`, functions and symbols are OMITTED from an object and
//    become `null` in an array — the one place the two containers differ.
// 3. NaN and both infinities serialize as `null`; every finite number uses
//    ToString(Number), so `1e21` is `1e+21` and `-0` is `0`.
// 4. The `space` argument produces exactly the indentation 25.5.2.3
//    describes, including the space after the colon that only appears when
//    the gap is non-empty.
// 5. A `toJSON` method replaces the value before serialization, and a
//    replacer FUNCTION is called with (key, value) starting at the empty key
//    for the root.
// 6. `parse` round-trips the output, rejects the JavaScript-only spellings
//    with a SyntaxError, and a reviver rebuilds values bottom-up.
const ordered = { b: 1, a: 2, nested: { z: [1, 2], y: "q" } };
console.log(JSON.stringify(ordered));

const gaps = { u: undefined, f: function () {}, n: null, ok: 1 };
console.log(JSON.stringify(gaps));
console.log(JSON.stringify([undefined, function () {}, null, 1]));

console.log(JSON.stringify([NaN, Infinity, -Infinity, -0, 1e21, 0.1]));
console.log(JSON.stringify("a\"b\\c\nd\tef"));

console.log(JSON.stringify(ordered, null, 2));
console.log(JSON.stringify({ a: [1] }, null, "\t"));

const dated = { at: { toJSON: function () { return "STAMP"; } }, n: 1 };
console.log(JSON.stringify(dated));
console.log(JSON.stringify({ a: 1, b: 2 }, function (k, v) {
  return k === "b" ? undefined : v;
}));

const back = JSON.parse('{"a":[1,2,{"b":"x"}],"c":true,"d":null}');
console.log(back.a[2].b, back.c, back.d, back.a.length);
console.log(JSON.stringify(JSON.parse(JSON.stringify(ordered))));
try {
  JSON.parse("{a:1}");
} catch (e) {
  console.log("SyntaxError for an unquoted key");
}
try {
  JSON.parse("[1,2,]");
} catch (e) {
  console.log("SyntaxError for a trailing comma");
}
console.log(JSON.parse('{"n":1,"m":2}', function (k, v) {
  return typeof v === "number" ? v * 10 : v;
}).n);
