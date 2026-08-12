// The Symbol primitive, end to end: the value model, symbol-keyed properties,
// the global registry, the well-known `Symbol.iterator`, and the refusal to
// coerce. Each of those has a case of its own — symbol_primitive.js,
// symbol_keys.js, symbol_registry.js, iterator_protocol.js — and this one is
// where they are pinned TOGETHER, because the property that matters about a
// symbol is that all five agree on one identity rule.
//
// From ECMA-262 6.1.5 (the Symbol type), 20.4.1 (the constructor), 20.4.2.1
// (Symbol.for), 20.4.2.5 (Symbol.iterator), 7.1.19 (ToPropertyKey) and 13.5.3
// (typeof):
//
// 1. `Symbol()` produces a value whose `typeof` is "symbol", and two calls
//    with the SAME description are two different values. That is the whole
//    point of the type and the thing a string key cannot do.
// 2. A symbol used as a property key is invisible to `Object.keys`, to
//    `for-in`, to `Object.entries` and to `JSON.stringify` — by BEING a
//    symbol, since all four are defined over string keys.
// 3. `Symbol.for` is a REGISTRY: it returns the same symbol for the same
//    string, where `Symbol()` never does, and `Symbol.keyFor` reverses it.
// 4. `Symbol.iterator` is a symbol rather than a string, so `typeof
//    Symbol.iterator` is "symbol" — and a program cannot make an object
//    iterable by assigning a STRING key, however that key is spelled. The
//    object below defines `"@@iterator"`, which was the key bronze used to
//    stand in for the well-known symbol, and it is not an iterable: matching
//    a symbol key by identity is what makes that spelling mean nothing.
// 5. A symbol does not coerce: `"" + sym` is a TypeError, and `toString`
//    is the only conversion, which is what makes an accidental
//    stringification loud instead of silent. The MESSAGE is not pinned —
//    only that it throws — because the wording is bronze's to choose.
const a = Symbol("tag");
const b = Symbol("tag");
console.log(typeof a);
console.log(a === b, a === a);
console.log(a.toString(), a.description);

const holder = {};
holder[a] = 1;
holder.plain = 2;
console.log(Object.keys(holder).join(","));
console.log(Object.entries(holder).length);
console.log(holder[a], holder[b]);
let seen = "";
for (const k in holder) seen = seen + k + ";";
console.log(seen);

console.log(Symbol.for("shared") === Symbol.for("shared"));
console.log(Symbol.for("shared") === Symbol("shared"));
console.log(Symbol.keyFor(Symbol.for("shared")), Symbol.keyFor(a));

console.log(typeof Symbol.iterator);
const notIterable = { "@@iterator": function () { return { next: function () {} }; } };
let iterated = false;
try {
  for (const x of notIterable) iterated = true;
} catch (e) {
  console.log("string key is not an iterator hook");
}
console.log(iterated);

try {
  console.log("" + a);
} catch (e) {
  console.log("a symbol does not coerce");
}
