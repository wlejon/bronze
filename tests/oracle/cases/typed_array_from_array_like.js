// 23.2.5.1 step 5, the arm that decides between the two ways a typed array can
// be built from an object. usingIterator is GetMethod(object, @@iterator); when
// it is present the constructor iterates (step 5.b), and when it is UNDEFINED
// it falls to step 5.c, InitializeTypedArrayFromArrayLike, which reads `length`
// and the indices and never asks for an iterator at all.
//
// So an object with no @@iterator is not an error — it is an array-like, and
// `{length: 2, 0: 1.5, 1: 2.5}` is a two-element view. An object with no
// `length` either is ToLength(undefined) = 0 and a length-ZERO view, which is
// what the `valueOf` line pins: an object with a `valueOf` and no `length` is
// an array-like of no elements, not a number in disguise.
//
// Each element goes through ToNumber, so a missing index is `undefined` and
// therefore NaN — and NaN stored into an integer view is 0, which is the
// difference the Uint8Array line shows against the Float64Array one. The
// conversion is per-kind (7.1.6..7.1.10), which is why 300 is 44 in a Uint8
// view and 255 in a Uint8Clamped one.
//
// The four construction paths that were already right are pinned beside it, so
// this case fails if the array-like arm ever swallows one of them: a real
// array, an iterable that is not one, the length form, and the buffer-copy form
// that another typed array takes.
const fromArrayLike = new Float64Array({ length: 2, 0: 1.5, 1: 2.5 });
console.log(fromArrayLike.length, fromArrayLike.join(","));
console.log(new Float64Array({ valueOf: function () { return 4; } }).length);
console.log(new Float64Array({}).length);
console.log(new Float64Array({ length: 2 }).join(","));
console.log(new Float64Array({ length: 3, 0: 1, 2: 4 }).join(","));

console.log(new Float64Array([1, 2]).join(","));
console.log(new Float64Array(new Set([1, 2])).join(","));
console.log(new Float64Array(4).join(","));
console.log(new Float64Array(new Float64Array([7, 8])).join(","));

// Every kind, over the one array-like.
const src = { length: 2, 0: 9, 1: 8 };
console.log(new Int8Array(src).join(","));
console.log(new Uint8Array(src).join(","));
console.log(new Uint8ClampedArray(src).join(","));
console.log(new Int16Array(src).join(","));
console.log(new Uint16Array(src).join(","));
console.log(new Int32Array(src).join(","));
console.log(new Uint32Array(src).join(","));
console.log(new Float32Array(src).join(","));
console.log(new Float64Array(src).join(","));

console.log(new Uint8Array({ length: 3, 0: 1, 2: 300 }).join(","));
console.log(new Uint8ClampedArray({ length: 2, 0: -5, 1: 300 }).join(","));
console.log(new Int16Array({ length: 2, 0: "5", 1: null }).join(","));

// Step 5.c reads each index with Get, so an accessor runs.
const withGetters = { length: 2 };
Object.defineProperty(withGetters, "0", {
  get: function () {
    return 1.25;
  },
  enumerable: true,
});
Object.defineProperty(withGetters, "1", {
  get: function () {
    return 6.5;
  },
  enumerable: true,
});
console.log(new Float64Array(withGetters).join(","));

// An object carrying BOTH takes the iterator path, and its `length` is ignored.
const both = { length: 1, 0: 42 };
both[Symbol.iterator] = function* () {
  yield 7;
  yield 8;
};
console.log(new Float64Array(both).join(","));
