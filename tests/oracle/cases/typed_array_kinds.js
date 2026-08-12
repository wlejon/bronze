// The nine views of ECMA-262 table 71, each built the three ways 23.2.5.1
// defines — from a LENGTH (step 4, a fresh zero-filled buffer), from an
// array-like (step 5.b.ii), and from ANOTHER typed array (step 5.b.i, which
// converts element by element rather than reinterpreting bytes).
//
// The conversion each store performs is the element type's, per 10.4.5.5
// IntegerIndexedElementSet: 7.1.6..7.1.10 for the integer kinds (truncate,
// then modulo 2^N, then re-sign), 7.1.11 for Uint8Clamped (saturate, round
// half to even), and a round-to-nearest narrowing for Float32.
//
// The print format is docs/0013 decision 1 extended by docs/0029: the
// constructor's name, the length in parentheses, then the elements.
console.log(new Int8Array(3));
console.log(new Int8Array([1, -2, 3]));
console.log(new Int8Array(new Float64Array([1.9, -2.9, 3.9])));

console.log(new Uint8Array(3));
console.log(new Uint8Array([1, 254, 255]));
console.log(new Uint8Array(new Int8Array([-1, -2, 127])));

console.log(new Uint8ClampedArray(3));
console.log(new Uint8ClampedArray([-5, 300, 7]));
console.log(new Uint8ClampedArray(new Float32Array([1.5, 2.5, 3.5])));

console.log(new Int16Array(2));
console.log(new Int16Array([32767, -32768]));
console.log(new Int16Array(new Int32Array([65536, 65535])));

console.log(new Uint16Array(2));
console.log(new Uint16Array([0, 65535]));
console.log(new Uint16Array(new Int8Array([-1, -2])));

console.log(new Int32Array(2));
console.log(new Int32Array([2147483647, -2147483648]));
console.log(new Int32Array(new Float64Array([2147483648, -1.5])));

console.log(new Uint32Array(2));
console.log(new Uint32Array([0, 4294967295]));
console.log(new Uint32Array(new Int32Array([-1, -2])));

console.log(new Float32Array(2));
console.log(new Float32Array([0.5, -1.25]));
console.log(new Float32Array(new Float64Array([0.1, 1e40])));

console.log(new Float64Array(2));
console.log(new Float64Array([0.1, -0.5]));
console.log(new Float64Array(new Float32Array([0.1])));

// 23.2.5.1 with no argument at all is a length of 0, and an empty view still
// prints what it is.
console.log(new Float32Array());
console.log(new Uint8Array(0));

// The element count, the byte count and the element width are three separate
// facts, and only for the one-byte kinds do the first two agree.
const f = new Float64Array(3);
console.log(f.length, f.byteLength, f.byteOffset, f.BYTES_PER_ELEMENT);
const s = new Int16Array(3);
console.log(s.length, s.byteLength, s.byteOffset, s.BYTES_PER_ELEMENT);
