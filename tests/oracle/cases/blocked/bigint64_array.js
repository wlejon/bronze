// BLOCKED: `unresolved name 'BigInt64Array': a ReferenceError if it is
// evaluated (bare `typeof BigInt64Array` is safe)`.
//
// The two 64-bit integer views (23.2, table 71). They are the only typed arrays
// whose element type is a BIGINT rather than a Number, and that one fact is
// what makes them a separate piece of work rather than two more rows in the
// existing table: 23.2.5.13 SetTypedArrayFromArrayLike goes through ToBigInt,
// not ToNumber, so a Number written into one is a TypeError (7.1.13 has no
// Number case) rather than a conversion — the one place in the language where
// a typed-array write can throw instead of silently truncating.
//
// Reads answer BigInts, so `a[0] === 5n` is true and `a[0] === 5` is false, and
// the wrap on store is the same modulo-2^64 the other views do (23.2.5.1's
// NumericToRawBytes over BigInt::asIntN / BigInt::asUintN), just at a width
// where no Number could hold the result exactly.
//
// Unblocking this means the typed-array machinery carrying an element type
// whose values are BigInts end to end: the constructor list, the element
// conversion on both directions, and `BYTES_PER_ELEMENT` of 8.

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
