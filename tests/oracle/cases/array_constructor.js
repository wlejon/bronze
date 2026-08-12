// The global `Array` as a VALUE: the bare name denotes one object, and
// everything below is a member of it or a fact about it.
//
// From ECMA-262:
//
// 1. 23.1.1.1 (Array) step 3: a single NUMBER argument is a LENGTH, and step
//    3.d sets only `length` — so `new Array(3)` has three ABSENT elements, not
//    three undefineds, and `0 in it` is false. Every other argument list is an
//    element list (step 4), so `new Array("3")` has length 1 and
//    `new Array(1, 2, 3)` has length 3. A number that is not a uint32 is step
//    3.b's RangeError.
// 2. The same 23.1.1.1 defines `Array(...)` and `new Array(...)`: NewTarget is
//    read only to choose a prototype, so the two spellings build the same array.
// 3. 23.1.2.2 (Array.isArray) is IsArray, which is true for an Array exotic
//    object and nothing else — a typed array (10.4.5), a string and a plain
//    object are all false.
// 4. 23.1.2.3 (Array.of): every argument becomes an element, INCLUDING a lone
//    number. That is the whole reason it exists beside `Array`.
// 5. 23.1.2.1 (Array.from): step 3 takes the iterator path when the source has
//    @@iterator — a string iterates by code point, a typed array and a Set by
//    element, a Map by entry — and step 4's array-like path otherwise, reading
//    `length` and then the indices. The mapping function receives (value, index)
//    and no receiver.
// 6. 10.2.5 / 23.1.3.1: `Array.prototype.constructor` is `Array`, so
//    `[].constructor === Array`, and it is the SAME object the bare name reads.
// 7. 13.10.2 InstanceofOperator over 23.1.3's `Array.prototype`: every array
//    inherits from it and nothing else does, so `x instanceof Array` and
//    `Array.isArray(x)` agree on every value — a typed array is not an Array
//    exotic object and a primitive has no prototype chain at all.

console.log(Array.isArray([]), Array.isArray([1, 2]));
console.log(Array.isArray({}), Array.isArray("ab"), Array.isArray(3));
console.log(Array.isArray(new Uint8Array(2)), Array.isArray(null));

const three = new Array(3);
console.log(three.length, three);
console.log(0 in three, three[0]);

const filled = new Array(2);
filled[0] = "x";
console.log(filled.length, filled, 0 in filled, 1 in filled);

console.log(new Array(1, 2, 3), new Array("3").length, new Array("3"));
console.log(new Array().length, Array(4).length, Array("a", "b"));

try {
  new Array(-1);
} catch (e) {
  console.log(e.name + ": " + e.message);
}
try {
  new Array(1.5);
} catch (e) {
  console.log(e.name + ": " + e.message);
}

console.log(Array.of(3), Array.of(3).length, new Array(3).length);
console.log(Array.of(), Array.of(1, "a", true));

console.log(Array.from([1, 2, 3]));
console.log(Array.from("abc"));
console.log(Array.from([1, 2, 3], function (v, i) { return v * 10 + i; }));
console.log(Array.from(new Uint8Array([7, 8])));
console.log(Array.from(new Set([1, 1, 2])));
console.log(Array.from(new Map([["a", 1]])));
console.log(Array.from({ length: 2, "0": "p", "1": "q" }));
console.log(Array.from({}), Array.from([]).length);

console.log([].constructor === Array, [1, 2].constructor === Array);
console.log(new Array(2).constructor === Array, typeof Array);

console.log([] instanceof Array, [1, 2] instanceof Array);
console.log(new Array(3) instanceof Array, Array.from("ab") instanceof Array);
console.log({} instanceof Array, "ab" instanceof Array, 3 instanceof Array);
console.log(new Uint8Array(1) instanceof Array);
