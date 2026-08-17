// Float16Array (ECMA-262 23.2, table 71) and Math.f16round (21.3.2.26): IEEE 754
// binary16, which is eleven significand bits and a five-bit exponent.
//
// Derived from IEEE 754 and ECMA-262, not from bronze's output. Each line below
// is a number a reader can check by hand:
//
// 1. A half's quantum at 1.0 is 2^-10, so 1.5 is exact and 0.1 is not:
//    0.1 / 2^-14 = 1638.4, which rounds to 1638 quanta = 0.0999755859375.
//    1/3 / 2^-12 = 1365.33, so 1365 quanta = 0.333251953125.
// 2. The largest finite half is (2 - 2^-10) * 2^15 = 65504. The next
//    representable magnitude would be 65536, so 65520 is the TIE between them —
//    and 6.1.6.1's round-to-nearest-EVEN sends a tie to the even significand,
//    which here is infinity. 65519 is below the tie and rounds to 65504.
// 3. The smallest positive half is the subnormal 2^-24 =
//    5.960464477539063e-8. Half of it, 2^-25, is the tie between 0 and 2^-24
//    and rounds to the even one: +0.
// 4. Signed zero survives a store, so 1 / a[i] tells -0 from +0.
// 5. The rounding must be computed from the DOUBLE, not by narrowing through a
//    float first. 2049 + 2^-30 is exactly representable as a double; in binary32
//    it rounds to exactly 2049, which is a binary16 TIE and would go to 2048 —
//    while the correct single rounding of the original value is above the tie and
//    gives 2050. That one line is the whole reason the conversion does not go
//    through `(float)`.
// 6. `Math.f16round(x)` and `new Float16Array([x])[0]` are the same number by
//    construction: they must not be able to disagree.
const a = new Float16Array(4);
console.log(a.length, a.BYTES_PER_ELEMENT, a.byteLength, a.byteOffset, a[0]);

a[0] = 1.5;
a[1] = 0.1;
a[2] = 65504;
a[3] = 65520;
console.log(a[0], a[1], a[2], a[3]);
console.log(a);

console.log(Math.f16round(1.5), Math.f16round(0.1), Math.f16round(1 / 3));
console.log(Math.f16round(65504), Math.f16round(65519), Math.f16round(65520));
console.log(Math.f16round(5.960464477539063e-8), Math.f16round(2.9802322387695312e-8));
console.log(Math.f16round(NaN), Math.f16round(Infinity), Math.f16round(-Infinity));

// The double-rounding witness. A conversion through binary32 answers 2048 here.
const witness = 2049 + 2 ** -30;
console.log(Math.f16round(witness), new Float16Array([witness])[0]);
console.log(Math.f16round(2049), Math.f16round(2051));

const signs = new Float16Array(2);
signs[0] = -0;
signs[1] = 0;
console.log(signs[0], 1 / signs[0], 1 / signs[1]);
signs[0] = NaN;
console.log(signs[0], signs[0] === signs[0]);

const from = new Float16Array([1, 2.5, -3.25, 0.1]);
console.log(from.length, from[3], String(from));
console.log([...from].length, from[0] === Math.f16round(1));

console.log(Object.prototype.toString.call(a), a.constructor === Float16Array);
console.log(Float16Array.BYTES_PER_ELEMENT, Float16Array.from([1.5, 2.5]).length);
console.log(Float16Array.of(0.1)[0] === Math.f16round(0.1));

// A view over a shared buffer: two bytes per element, so a 4-byte buffer holds
// two halves and the Uint8Array over the same bytes sees the stored pattern.
const buf = new ArrayBuffer(4);
const halves = new Float16Array(buf);
halves[0] = 1;
console.log(halves.length, new Uint8Array(buf)[0], new Uint8Array(buf)[1]);

// The two 25.3.4 accessors the same proposal adds to DataView. They share
// typed_array.cpp's binary16 conversion, so the byte pattern of 1.0 written
// through the view is the same 0x3C00 an element store produces — and a
// getFloat16 over the element store reads back the identical double.
const dv = new DataView(buf);
dv.setFloat16(2, 0.1, true);
console.log(dv.getFloat16(2, true), dv.getFloat16(2, true) === halves[1]);
console.log(dv.getFloat16(0, true) === 1, new Uint8Array(buf)[3]);
dv.setFloat16(0, NaN, true);
console.log(new Uint8Array(buf)[1], Number.isNaN(dv.getFloat16(0, true)));
