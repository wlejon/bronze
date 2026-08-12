// A symbol as a PROPERTY KEY: ECMA-262 7.1.19 (ToPropertyKey), 6.1.7.1
// (OwnPropertyKeys) and 7.3.23 (EnumerableOwnProperties).
//
// The value model behind this is the interesting part. A shape's transition key
// is an arena-interned string compared by CONTENT, and that content comparison
// is exactly what lets two objects with the same property names share a hidden
// class. A symbol key is the opposite — compared by IDENTITY, so the two keys
// below, which have the same description, are two different properties on the
// same object. Both facts are pinned here, on one object, because a
// representation that got either alone would look right.
//
// The invisibility half is 7.3.23's `key-of-type-String` filter and nothing
// else. A symbol-keyed property is absent from `Object.keys`, `Object.entries`,
// `for-in`, `JSON.stringify` and `getOwnPropertyNames` because it is a SYMBOL,
// not because of anything about how it is spelled. The other way an object can
// carry state nothing enumerates is an INTERNAL SLOT, which is not a property
// at all; collection_internal_slots.js pins that the two mechanisms stay
// apart, and in particular that a slot is absent from
// `getOwnPropertySymbols` where a symbol key is present.
const key = Symbol("k");
const other = Symbol("k");

const o = { plain: 1 };
o[key] = "first";
o[other] = "second";
o.later = 2;

// Same description, different key. And the STRING that a symbol's description
// would produce names nothing.
console.log(o[key], o[other], o.plain, o.later);
console.log(o["Symbol(k)"], o["k"]);

console.log(Object.keys(o).join(","));
console.log(Object.values(o).join(","));
console.log(Object.entries(o).length);
console.log(JSON.stringify(o));
console.log(Object.getOwnPropertyNames(o).join(","));
let seen = "";
for (const k in o) seen = seen + k + ";";
console.log(seen);

// 20.1.2.11 getOwnPropertySymbols. 6.1.7.1 orders symbol keys after every
// string key and among themselves in property-CREATION order — which is the
// order they were added to THIS object, not the order the symbols were made.
const syms = Object.getOwnPropertySymbols(o);
console.log(syms.length, syms[0] === key, syms[1] === other);
console.log(key in o, other in o, Symbol("k") in o);
console.log(Object.hasOwn(o, key), Object.hasOwn(o, Symbol("k")));
console.log(o);

// 7.3.25 CopyDataProperties takes OwnPropertyKeys rather than the string half
// of it, so spread and `Object.assign` are the one enumeration that carries a
// symbol-keyed property across.
const copy = { ...o };
console.log(Object.keys(copy).join(","), copy[key], copy[other]);
const assigned = Object.assign({}, o);
console.log(assigned[key], Object.getOwnPropertySymbols(assigned).length);

// 6.1.7.1's three groups on one object: integer-like keys ascending, then the
// remaining string keys in creation order, then the symbols.
const mixed = {};
const s1 = Symbol("s1");
mixed[s1] = "sym";
mixed.b = "b";
mixed[2] = "two";
mixed[1] = "one";
console.log(Object.getOwnPropertyNames(mixed).join(","));
console.log(Object.getOwnPropertySymbols(mixed).length);
console.log(mixed);

// The prototype chain finds a symbol key exactly as it finds a string one, and
// an own one shadows it without becoming an own key of the prototype.
const protoKey = Symbol("proto");
const parent = {};
parent[protoKey] = "inherited";
const child = Object.create(parent);
console.log(child[protoKey], Object.getOwnPropertySymbols(child).length, protoKey in child);
child[protoKey] = "own";
console.log(child[protoKey], parent[protoKey], Object.getOwnPropertySymbols(child).length);

// 13.5.1 delete, which removes the own property and leaves the other one — the
// two keys are different properties, so removing one says nothing about the
// other. It also moves the object to dictionary mode, so everything above is
// asked again on the other side of that transition.
console.log(delete o[key]);
console.log(o[key], o[other], Object.getOwnPropertySymbols(o).length);
console.log(Object.keys(o).join(","), JSON.stringify(o));
o[key] = "re-added";
console.log(o[key], Object.getOwnPropertySymbols(o).length);

// Instances of one class share a hidden class, and a symbol-keyed property has
// to take a shape transition like any other for that to hold. Nine keys, so
// the slots run past the four inline ones into the overflow block.
const slots = [];
for (let i = 0; i < 9; i++) slots.push(Symbol("slot" + i));
function Bag(base) {
  for (let i = 0; i < slots.length; i++) this[slots[i]] = base + i;
  this.name = "bag";
}
const p = new Bag(100);
const q = new Bag(200);
let sum = 0;
for (let i = 0; i < slots.length; i++) sum = sum + p[slots[i]] + q[slots[i]];
console.log(sum, p.name, q.name);
console.log(Object.keys(p).join(","), Object.getOwnPropertySymbols(q).length);
console.log(p[slots[8]], q[slots[0]], p[slots[0]] === q[slots[0]]);

// A symbol key on a FUNCTION lands in the same side object its statics use, so
// a static and a symbol-keyed property coexist.
function Marked() {}
Marked.plainStatic = 1;
Marked[key] = "on the function";
console.log(Marked[key], Marked.plainStatic, key in Marked);

// A function that was never given a static has no side object yet, so the
// symbol-keyed write is what builds it. Nothing else in a program with only
// symbol keys on it would.
function Bare() {}
Bare[other] = "bare";
console.log(Bare[other], Object.getOwnPropertySymbols(Bare).length);
console.log(delete Bare[other], Bare[other], Object.getOwnPropertySymbols(Bare).length);

// A receiver with no shape has nowhere to keep a symbol key, and a write to one
// is refused by name — so the empty answer here is complete rather than a gap.
console.log(Object.getOwnPropertySymbols([1]).length,
            Object.getOwnPropertySymbols(new Map()).length,
            Object.getOwnPropertySymbols(function () {}).length);
