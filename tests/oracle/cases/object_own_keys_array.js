// `Object.hasOwn` on an ARRAY: 20.1.2.13, which asks only whether a key names
// an own property of the object.
//
// An array's own keys are its ELEMENTS and a `length` that bronze keeps in the
// array header rather than in a shape, and that fact is why
// `Object.getOwnPropertyNames`, `Object.getOwnPropertyDescriptor` and
// `Object.defineProperty` all refuse an array: there is nowhere to write a
// descriptor and no key list to build. `hasOwn` needs neither. It needs the
// existence test the `in` operator already runs -- so it was refused for a
// reason true of its neighbours and not of it, and the language's answer to
// `Object.hasOwn([1, 2], 0)` is `true`.
//
// Those neighbours are still refused, and that half cannot be shown here: it is
// a hard error rather than a catchable one, so `tests/runtime/object_test.cpp`
// holds it. This case is what a program can see.
//
// What each line pins, from 10.4.2 (Array exotic objects) and 6.1.7.1:
//
// 1. An index below `length` is an own key; one at or past it is not, and
//    neither is a negative one.
// 2. The key is compared as TEXT after ToPropertyKey, so `0` and `"0"` are the
//    same question, while `"00"` and `"0.0"` are not canonical numeric strings
//    and name nothing.
// 3. `length` is an own property of every array. A name reached through
//    `Array.prototype` -- `toString` -- is not an OWN one, which is the whole
//    difference between `hasOwn` and `in`.
// 4. A HOLE is not an own key: `delete a[1]` takes index 1 out of them without
//    moving `length`, and that is the same set `Object.keys` and `for-in`
//    report.
// 5. An element whose VALUE is `undefined` still is one. The question is about
//    the key, and answering it from the value is how `a[1] = undefined` and
//    `delete a[1]` would come to look alike.
// 6. `hasOwn` is the OWN half of `in`, so the two agree about every key an
//    array can own. Where they part company is the prototype chain, and that
//    is not reachable here: bronze has no `Array.prototype` object yet
//    (`cases/get_prototype_of_primitive` names the gap), so `"toString" in a`
//    is a question about that gap and not about this one.

const a = [10, 20, 30];

console.log(Object.hasOwn(a, 0), Object.hasOwn(a, 2), Object.hasOwn(a, 3), Object.hasOwn(a, -1));
console.log(Object.hasOwn(a, "0"), Object.hasOwn(a, "00"), Object.hasOwn(a, "0.0"));
console.log(Object.hasOwn(a, "length"), Object.hasOwn(a, "toString"), Object.hasOwn(a, "nope"));
console.log(Object.hasOwn([], 0), Object.hasOwn([], "length"));

delete a[1];
console.log(a.length, Object.hasOwn(a, 1), 1 in a, Object.keys(a).join(","));

const undef = [1, undefined, 3];
console.log(Object.hasOwn(undef, 1), undef[1], undef.length);

// `hasOwn` is the own half of `in`, so the two agree about every key an array
// can own -- an element, a hole, one past the end, and `length`.
console.log(Object.hasOwn(a, 0) === (0 in a), Object.hasOwn(a, 1) === (1 in a));
console.log(Object.hasOwn(a, 3) === (3 in a), Object.hasOwn(a, "length") === ("length" in a));
