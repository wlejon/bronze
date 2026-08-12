// BLOCKED on ONE line: `typeof Symbol.iterator` answers "string".
//
// The symbol PRIMITIVE has landed. `Symbol()`, symbol-keyed properties,
// `Symbol.for`/`keyFor`, `getOwnPropertySymbols` and the refusal to coerce are
// all built and pinned in cases/symbol_primitive.js, symbol_keys.js and
// symbol_registry.js — the value-model problem this case's header used to
// describe is solved, by a one-pointer `PropertyKey` (runtime/property_key.h)
// that matches a string key by content and a symbol key by identity, and by
// putting a symbol in the non-moving arena so a shape node may point at it.
//
// What is left is the WELL-KNOWN symbols, and specifically `Symbol.iterator`,
// which is still the string `"@@iterator"`. That migration is not one line of
// the value model. The `@@` prefix rule — an own key beginning with `@@` is
// created non-enumerable — is load-bearing in three separate places and only
// one of them is the iterator hook:
//
//  - `Symbol.iterator` itself, read by the for-of and spread paths and written
//    by a program that makes an object iterable (src/parse/parser_generator.cpp
//    lowers `[Symbol.iterator]` to the string at compile time);
//  - the generator desugaring's self-property, which every `function*` object
//    carries so that iterating one twice works;
//  - the internal slots `Map`, `Set`, `String.prototype.matchAll` and the typed
//    arrays keep on their iterator objects — `@@mapTarget`, `@@mapCursor`,
//    `@@matchAllInput` and friends — which are not iterator hooks at all and
//    only use the rule to stay out of enumeration.
//
// Moving one without the others breaks the other two silently, which is why
// cases/collection_internal_slots.js exists and why this is a chunk of its own.
//
// What this case still pins, from ECMA-262 6.1.5 (the Symbol type), 20.4.1 (the
// constructor), 20.4.2.1 (Symbol.for) and 7.1.19 (ToPropertyKey) — points 1, 2,
// 3 and 5 pass today and are here as the promoted case's regression cover;
// point 4 is the blocker:
//
// 1. `Symbol()` produces a value whose `typeof` is "symbol", and two calls
//    with the SAME description are two different values. That is the whole
//    point of the type and the thing a string key cannot do.
// 2. A symbol used as a property key is invisible to `Object.keys`, to
//    `for-in`, to `Object.entries` and to `JSON.stringify` — by being a symbol,
//    which is what the `@@` prefix rule only ever approximated.
// 3. `Symbol.for` is a REGISTRY: it returns the same symbol for the same
//    string, where `Symbol()` never does, and `Symbol.keyFor` reverses it.
// 4. `Symbol.iterator` is a symbol rather than a string, so `typeof
//    Symbol.iterator` is "symbol" and a user cannot make an object iterable
//    by assigning a string key. Both halves of that are divergences pinned
//    the other way round in cases/iterator_protocol.js today, and both are
//    what the migration above has to change.
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
