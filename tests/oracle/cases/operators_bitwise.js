// `&`, `|`, `^`, `~` and their compound assignments.
//
// Every operand goes through ToInt32 (ECMA-262 7.1.6) before a single bit is
// touched: the double is truncated toward zero, reduced modulo 2^32, and
// reinterpreted as a signed 32-bit integer. NaN, both infinities and both
// zeroes therefore all become 0, and 2147483648 wraps to -2147483648. The
// result is that int32 read back as a JS number, so the printed values below
// are derived from the 32-bit patterns, not from the doubles that went in.
console.log(5 & 3);
console.log(5 | 3);
console.log(5 ^ 3);
console.log(~5);
console.log(~0);
console.log(~-1);

// ~x is -ToInt32(x) - 1, so a double is truncated first: ~3.7 is ~3 is -4,
// and ~~3.7 is 3 - the idiomatic "truncate to int32" spelling.
console.log(~3.7);
console.log(~~3.7);
console.log(~~-3.7);

// Negative operands are the two's-complement patterns:
//   -5 is 0xFFFFFFFB, so -5 & 3 is 3, -5 | 3 is -5, and -5 ^ 3 is 0xFFFFFFF8.
console.log(-5 & 3);
console.log(-5 | 3);
console.log(-5 ^ 3);

// ToInt32 of the values that have no int32 at all.
console.log(NaN | 0);
console.log(Infinity | 0);
console.log(-Infinity | 0);
console.log(-0 | 0);

// The modulo-2^32 wraparound, at and past both ends of the range.
console.log(2147483647 | 0);
console.log(2147483648 | 0);
console.log(4294967295 | 0);
console.log(4294967296 | 0);
console.log(4294967297 | 0);
console.log(-2147483649 | 0);
console.log(3000000000 | 0);

// Truncation toward zero, not floor.
console.log(1.9 | 0);
console.log(-1.9 | 0);

// A non-number operand is ToNumber'd first, then ToInt32'd: "12" is 12,
// a string that is not a numeric literal is NaN and so 0, null is 0, and
// undefined is NaN and so 0. Booleans are 1 and 0.
console.log("12" & 10);
console.log("abc" | 0);
console.log(null | 0);
console.log(undefined | 0);
console.log(true & 1);
console.log(true | false);

// Precedence among the three: & binds tighter than ^, which binds tighter
// than |. So 1 | 2 ^ 3 & 3 is 1 | (2 ^ (3 & 3)) is 1 | 1 is 1.
console.log(1 | 2 ^ 3 & 3);
console.log(1 & 2 | 3);
// Equality binds tighter than any of them: 2 | 0 == 0 is 2 | true is 3.
console.log(2 | 0 == 0);

// The result is an ordinary number and keeps working like one.
console.log((5 & 3) + 1);
console.log((5 | 3) / 7);

let a = 12;
a &= 10;
console.log(a);
a |= 5;
console.log(a);
a ^= 3;
console.log(a);

// Compound assignment through a property and an element, which read the
// target before the right-hand side is evaluated.
const o = { flags: 6 };
o.flags &= 3;
console.log(o.flags);
o.flags |= 8;
console.log(o.flags);
const arr = [7];
arr[0] ^= 5;
console.log(arr[0]);
