// A typed array constructor is a VALUE, not just a spelling `new` recognises.
// ECMA-262 10.2.5 puts a `constructor` back-pointer on every instance, and
// 23.2.6.2 puts `BYTES_PER_ELEMENT` on the constructor itself, so all of the
// following have to be the same object and `===` has to say so.
//
// This is the shape three.js's `math/MathUtils.js` `denormalize` is written
// in: a `switch` whose cases are constructors, matched against
// `array.constructor` with strict equality. If the bare name and the
// back-pointer were two different objects, every case would fall through to
// the default and every normalised value would be wrong — silently.
const f = new Float32Array(2);
console.log(f.constructor === Float32Array);
console.log(f.constructor === Uint32Array);
console.log(Float32Array === Float32Array);
console.log(Int8Array === Uint8Array);

function scale(array, value) {
  switch (array.constructor) {
    case Float32Array:
      return value;
    case Uint32Array:
      return value / 4294967295;
    case Uint16Array:
      return value / 65535;
    case Uint8Array:
      return value / 255;
    case Int32Array:
      return Math.max(value / 2147483647, -1);
    case Int16Array:
      return Math.max(value / 32767, -1);
    case Int8Array:
      return Math.max(value / 127, -1);
    default:
      return -999;
  }
}
console.log(scale(new Float32Array(1), 0.25));
console.log(scale(new Uint8Array(1), 255));
console.log(scale(new Uint16Array(1), 65535));
console.log(scale(new Int8Array(1), -127));
console.log(scale(new Uint8ClampedArray(1), 1));

// 23.2.6.2 on the constructor, and 23.2.7.2 (the same number) on an instance.
console.log(Int8Array.BYTES_PER_ELEMENT, Uint8ClampedArray.BYTES_PER_ELEMENT);
console.log(Int16Array.BYTES_PER_ELEMENT, Uint32Array.BYTES_PER_ELEMENT);
console.log(Float32Array.BYTES_PER_ELEMENT, Float64Array.BYTES_PER_ELEMENT);
console.log(new Uint16Array(1).BYTES_PER_ELEMENT);

// The back-pointer used the way three.js's `BufferAttribute.copy` uses it:
// clone an array without naming its class. The callee here is a member
// expression, which no name-recognition at the `new` could ever have reached.
function cloneOf(array) {
  return new array.constructor(array);
}
const original = new Int16Array([4, 5, 6]);
const clone = cloneOf(original);
original[0] = 0;
console.log(clone, clone.constructor === Int16Array);

// The nine constructors held in an object, which is `utils.js`'s TYPED_ARRAYS
// table: reading one back out has to yield the same object again.
const table = {
  Int8Array: Int8Array,
  Uint8Array: Uint8Array,
  Uint8ClampedArray: Uint8ClampedArray,
  Int16Array: Int16Array,
  Uint16Array: Uint16Array,
  Int32Array: Int32Array,
  Uint32Array: Uint32Array,
  Float32Array: Float32Array,
  Float64Array: Float64Array,
};
console.log(table.Float64Array === Float64Array);
console.log(new table.Uint8Array([1, 2]));
console.log(typeof Float32Array, typeof ArrayBuffer);

// `ArrayBuffer` is a global constructor object on the same terms.
const buffer = new ArrayBuffer(4);
console.log(buffer.constructor === ArrayBuffer);
console.log(new Uint8Array(buffer).buffer.constructor === ArrayBuffer);
