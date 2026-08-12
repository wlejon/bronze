// BLOCKED: `unsupported: Symbol() (bronze has no symbol primitive;
// Symbol.iterator is a well-known string key)`.
//
// The iterator protocol shipped WITHOUT a symbol primitive, and this case is
// the receipt for that trade. `Symbol.iterator` is the string "@@iterator", and
// the compensating rule — any own key beginning with `@@` is created
// non-enumerable — buys back the one property of a symbol key that the protocol
// depends on. It buys nothing else, and this case pins what is still missing.
//
// The blocker is the value model, not the syntax. Tag 0xFFF8 is reserved for
// Symbol and free, so there is room for the VALUE; the cost is
// everywhere a property KEY is handled. A shape's transition key is an
// arena-interned `StringHeader*` compared by CONTENT, and that
// content comparison is what makes two objects with the same property names
// share a shape. A symbol key is the opposite: it is compared by IDENTITY,
// and two symbols with the same description are two different keys. So
// landing symbols means one of:
//
//  - a parallel key type threaded through shapes, dictionaries, enumeration,
//    the inline caches and `Object.keys` — every one of which currently
//    assumes "key" means "interned string"; or
//  - a pointer-identity rule for symbol-keyed transitions only, which makes
//    property matching two rules instead of one and has to be right in the
//    inline cache fast path that generated code open-codes.
//
// Either is a value-model chunk of its own, which is why it is not this one.
//
// What this case pins when it lands, from ECMA-262 6.1.5 (the Symbol type),
// 20.4.1 (the Symbol constructor), 20.4.2.1 (Symbol.for) and 7.1.19
// (ToPropertyKey):
//
// 1. `Symbol()` produces a value whose `typeof` is "symbol", and two calls
//    with the SAME description are two different values. That is the whole
//    point of the type and the thing a string key cannot do.
// 2. A symbol used as a property key is invisible to `Object.keys`, to
//    `for-in`, to `Object.entries` and to `JSON.stringify` — which bronze
//    approximates today with the `@@` prefix rule, and which the real thing
//    gets for free.
// 3. `Symbol.for` is a REGISTRY: it returns the same symbol for the same
//    string, where `Symbol()` never does, and `Symbol.keyFor` reverses it.
// 4. `Symbol.iterator` is a symbol rather than a string, so `typeof
//    Symbol.iterator` is "symbol" and a user cannot make an object iterable
//    by assigning a string key. Both halves of that are divergences pinned
//    the other way round in cases/iterator_protocol.js today.
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
