// BigInt64Array and BigUint64Array (ECMA-262 23.2, table 71): the two views
// whose element type is a BIGINT rather than a Number.
//
// Promoted from cases/blocked/ unchanged: the expectation below is the file that
// was pinned there, byte for byte, and every line of it was derived from the
// specification before bronze could run any of them.
//
// What it pins, and why each line is the one fact worth pinning:
//
// 1. `BYTES_PER_ELEMENT` is 8 on the instance and on the constructor, and a
//    fresh view reads 0n — `String(a[0])` is "0", because the element is a
//    BigInt and BigInt::toString has no `n` in it.
// 2. A read answers a BIGINT, so `a[0] === 5n` is true and `a[0] === 5` is
//    FALSE. 7.2.15 does not convert across the two numeric types, so the second
//    is not a near miss — it is a type mismatch.
// 3. The store wraps modulo 2^64 (23.2.5.1's NumericToRawBytes over
//    BigInt::asIntN / asUintN), at a width where no Number could hold either
//    answer exactly: 2n ** 63n stores as the most negative Int64, and -1n in the
//    unsigned view reads as 2^64 - 1.
// 4. `a[0] = 1` is a TypeError and not a conversion. 7.1.13 ToBigInt's table has
//    no Number row, which makes this the ONE place a typed-array write can throw
//    instead of silently truncating — and the reason these two rows are a
//    separate element KIND in bronze rather than two more widths.
// 5. `BigInt64Array.from` builds one from an array of BigInts, so the static
//    reaches the same ToBigInt store the element path does.
//
// Not pinned here because bronze refuses them by name: 23.2.3's PROTOTYPE
// methods over a BigInt view. Every one of them in this runtime converts an
// element through a `double`, which cannot carry a 64-bit integer, so
// `a.fill(1n)` is a named diagnostic rather than a wrong answer.
const a = new BigInt64Array(2);
console.log(a.length, a.BYTES_PER_ELEMENT, String(a[0]));

a[0] = 5n;
a[1] = -1n;
console.log(String(a[0]), String(a[1]), a[0] === 5n, a[0] === 5);

// Wraps at 2^63, exactly as the specification's BigInt::asIntN says.
a[0] = 2n ** 63n;
console.log(String(a[0]));

const u = new BigUint64Array(1);
u[0] = -1n;
console.log(String(u[0]), u.BYTES_PER_ELEMENT);

// A Number is not a BigInt, and ToBigInt says so rather than converting.
try {
  a[0] = 1;
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError);
}

console.log(BigInt64Array.BYTES_PER_ELEMENT, BigInt64Array.from([1n, 2n]).length);
