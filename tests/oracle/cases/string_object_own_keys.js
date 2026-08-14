// The own keys of a String exotic object (ECMA-262 10.4.3.3, 10.4.3.4,
// 10.4.3.5).
//
// `cases/primitive_wrapper_objects` built the wrapper and `cases/string_index`
// pinned reading an index off one through 10.4.3.5 StringGetOwnProperty. This
// case pins the other direction — 10.4.3.3 OwnPropertyKeys — across every
// operation that asks an object what its own keys are:
//
// 1. The own keys are the indices in ascending order and then `length`, which
//    is the order 10.4.3.3 states rather than creation order.
// 2. `Object.keys` sees the indices and not `length`, because 10.4.3.4 defines
//    `length` non-enumerable and 10.4.3.5 defines an index enumerable. That
//    one difference is why both lines below are here.
// 3. An index is a real own property, so `hasOwnProperty` and `in` answer true
//    for one inside the length and false for one past it.
// 4. Its descriptor is non-writable and non-configurable and enumerable — the
//    exact attribute set 10.4.3.5 names, and the reason no program can shadow
//    an index property or delete one.
// 5. `for-in` visits the indices and nothing else: everything on
//    `String.prototype` and `Object.prototype` above it is non-enumerable.

const s = new String("ab");

console.log(Object.keys(s).join(","));
console.log(Object.getOwnPropertyNames(s).join(","));

console.log(s.hasOwnProperty("0"), s.hasOwnProperty("length"), s.hasOwnProperty("2"));
console.log("0" in s, "length" in s, "2" in s);
console.log(s.propertyIsEnumerable("0"), s.propertyIsEnumerable("length"));

const d = Object.getOwnPropertyDescriptor(s, "0");
console.log(d.value, d.writable, d.enumerable, d.configurable);

const seen = [];
for (const k in s) seen.push(k);
console.log(seen.join(","));
