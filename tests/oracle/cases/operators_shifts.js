// `<<`, `>>`, `>>>` and their compound assignments (docs/0015 decision 2).
//
// The left operand is ToInt32'd like any other bitwise operand; the shift
// COUNT is ToUint32'd and masked to its low five bits, so a count of 32 is a
// count of 0 and a count of -1 is a count of 31. `>>` keeps the sign bit,
// `>>>` does not - and `>>>` is the one operator here whose result is
// ToUint32 rather than ToInt32, which is why -1 >>> 0 is 4294967295 and not
// -1 (ECMA-262 13.9).
console.log(1 << 3);
console.log(16 >> 2);
console.log(5 >> 1);
console.log(8 >>> 2);

// Sign: >> replicates the sign bit, >>> shifts zeroes in.
//   -16 is 0xFFFFFFF0; >> 2 is 0xFFFFFFFC (-4); >>> 2 is 0x3FFFFFFC.
console.log(-16 >> 2);
console.log(-16 >>> 2);
console.log(-5 >> 1);
console.log(-5 >>> 1);
console.log(-1 >> 0);
console.log(-1 >>> 0);
console.log(1 >>> 0);

// A left shift into and past the sign bit, with the int32 wraparound.
console.log(1 << 30);
console.log(1 << 31);
console.log(3 << 30);

// The count is masked to five bits, so 32 shifts by 0 and 33 by 1. A
// negative count is ToUint32'd first: -1 becomes 4294967295, whose low five
// bits are all ones, so it shifts by 31.
console.log(1 << 32);
console.log(1 << 33);
console.log(-1 >>> 32);
console.log(1 << -1);
console.log(2 >> 1.9);

// Non-number operands, on both sides.
console.log("8" >> 1);
console.log(1 << "3");
console.log(NaN << 3);
console.log(8 >> NaN);

// Precedence: shifts are looser than + and *, tighter than the relational
// operators. So 1 << 2 + 3 is 1 << 5, and 4 >> 1 < 3 is (4 >> 1) < 3.
console.log(1 << 2 + 3);
console.log(1 << 2 * 3);
console.log(4 >> 1 < 3);
// ... and looser than the shifts themselves, left to right:
// 256 >> 2 >> 2 is (256 >> 2) >> 2.
console.log(256 >> 2 >> 2);

let s = 1;
s <<= 4;
console.log(s);
s >>= 2;
console.log(s);
s = -8;
s >>>= 1;
console.log(s);

const o = { bits: 1 };
o.bits <<= 5;
console.log(o.bits);
