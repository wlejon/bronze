// `Object.prototype` as a REAL object on the real chain: every plain object's
// root shape names it, so the prototype walk that already found a class's
// methods finds these, and a program can hold the object, compare it, and add
// to it. That is the difference this case exists to pin — an array, a number
// and a function still have their members handed out BESIDE them by the
// property path, where nothing can reach the holder, and
// `Object.getPrototypeOf` still says so by name for those. A string and a
// boolean have joined this side of the line
// (`cases/get_prototype_of_primitive`).
//
// The property that made this safe to introduce under a suite of pinned
// expectations is on line 1 of 20.1.3: every member of `Object.prototype` is
// non-enumerable. `for-in` walks the prototype chain, so an enumerable one
// would have appeared in every for-in over every object in the program; the
// last two lines below are the guard for that, and they are the reason this
// case is not only about the members it adds.
//
// From ECMA-262 20.1.2.12 (getPrototypeOf), 20.1.2.13 (hasOwn), 20.1.2.14
// (is), 20.1.2.9 (getOwnPropertyDescriptors) and 20.1.3 (Object.prototype):
//
// 1. A plain `{}` inherits from `Object.prototype`, and `Object.prototype`
//    itself inherits from nothing.
// 2. `Object.is` is SameValue, so it separates `0` from `-0` where `===` does
//    not, and joins NaN to itself where `===` does not.
// 3. `hasOwn` asks about OWN properties only, so an inherited one answers
//    false where `in` answers true.
// 4. A descriptor map reports every own property, with the three attributes a
//    plain assignment gives.
// 5. Nothing on `Object.prototype` is enumerable, so the chain it put under
//    every object in the program is invisible to the operations that walk one.
console.log(Object.getPrototypeOf({}) === Object.prototype);
console.log(Object.getPrototypeOf(Object.prototype));
console.log(Object.prototype.hasOwnProperty.call({ a: 1 }, "a"));

console.log(Object.is(NaN, NaN), NaN === NaN);
console.log(Object.is(0, -0), 0 === -0);
console.log(Object.is(1, 1), Object.is("a", "a"));

const child = Object.create({ inherited: 1 });
child.own = 2;
console.log(Object.hasOwn(child, "own"), Object.hasOwn(child, "inherited"));
console.log("inherited" in child);

const map = Object.getOwnPropertyDescriptors({ a: 1, b: 2 });
console.log(map.a.value, map.a.writable, map.a.enumerable, map.a.configurable);
console.log(Object.keys(map).join(","));

// `for-in` is the strict one of the three: 14.7.5.9 EnumerateObjectProperties
// visits inherited keys as well as own ones, so it is the only operation here
// that reaches `Object.prototype` at all — and it must come back with exactly
// the two the literal wrote. `Object.keys` (20.1.2.17) and `JSON.stringify`
// (25.5.2) ask for own enumerable keys and so never left the object.
const seen = [];
for (const k in { x: 1, y: 2 }) seen.push(k);
console.log(seen.join(","));
console.log(Object.keys({ x: 1 }).join(","), JSON.stringify({ x: 1 }));
