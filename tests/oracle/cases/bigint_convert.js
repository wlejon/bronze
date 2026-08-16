// The conversions in and out of BigInt: 21.2.1.1 BigInt(value), 7.1.14
// StringToBigInt, 6.1.6.2's ToString and its ℝ -> Number, and the two
// operations that REFUSE (ToNumber and JSON).
function fail(f) {
  try { return String(f()); } catch (e) {
    const kind = e instanceof RangeError ? "RangeError"
               : e instanceof SyntaxError ? "SyntaxError"
               : e instanceof TypeError ? "TypeError" : "Error";
    return kind + ": " + e.message;
  }
}

// 21.2.1.1: a Number goes through NumberToBigInt, which accepts an INTEGER
// and refuses everything else rather than truncating.
console.log(BigInt(42), BigInt(-42), BigInt(0), BigInt(-0), BigInt(9007199254740992));
console.log(BigInt(true), BigInt(false));
console.log(fail(() => BigInt(1.5)));
console.log(fail(() => BigInt(NaN)));
console.log(fail(() => BigInt(Infinity)));
console.log(fail(() => BigInt(null)));
console.log(fail(() => BigInt(undefined)));
console.log(fail(() => BigInt(Symbol("s"))));

// 7.1.14 StringToBigInt: the numeric literal grammar minus the fraction, the
// exponent and the suffix, plus a leading sign. The empty string is 0n.
console.log(BigInt("42"), BigInt("  42  "), BigInt(""), BigInt("   "));
console.log(BigInt("+5"), BigInt("-5"), BigInt("0x10"), BigInt("0b1011"), BigInt("0o17"));
console.log(BigInt("123456789012345678901234567890"));
console.log(fail(() => BigInt("1.5")));
console.log(fail(() => BigInt("1e3")));
console.log(fail(() => BigInt("x")));
console.log(fail(() => BigInt("-0x10")));

// Step 2 is ToPrimitive with hint number, so an object converts through it.
console.log(BigInt({ valueOf() { return 5n; } }), BigInt({ valueOf() { return "7"; } }));

// 6.1.6.2's ℝ -> Number rounds to nearest, ties to even, which is why
// 2^53+1 lands below and 2^53+3 lands above.
console.log(Number(1n), Number(-1n), Number(0n), Number(2n ** 53n));
console.log(Number(2n ** 53n + 1n), Number(2n ** 53n + 3n));
console.log(Number(2n ** 1024n), Number(-(2n ** 1024n)), Number(2n ** 1023n));

// ToNumber is NOT that conversion: it refuses, which is what makes the
// implicit coercions loud.
console.log(fail(() => 1n * 1));
console.log(fail(() => Math.sqrt(4n)));
console.log(fail(() => "ab".repeat(2n)));

// 21.2.3.3 toString(radix), and 21.2.3.4 valueOf.
console.log((255n).toString(), (255n).toString(16), (255n).toString(2), (255n).toString(36));
console.log((-255n).toString(16), (0n).toString(36), (5n).valueOf(), typeof (5n).valueOf());
const big = 1234567890123456789012345678901234567890n;
console.log(BigInt("0x" + big.toString(16)) === big, BigInt("0b" + big.toString(2)) === big);
console.log(fail(() => (1n).toString(1)));
console.log(fail(() => (1n).toString(37)));

// ToBoolean is total and 0n is the only falsy BigInt.
console.log(Boolean(0n), Boolean(1n), Boolean(-1n), !!0n, 0n ? "y" : "n", 1n ? "y" : "n");

// 25.5.2: a BigInt has no JSON representation, and the refusal is catchable.
console.log(fail(() => JSON.stringify(1n)));
console.log(fail(() => JSON.stringify({ a: 1n })));
console.log(JSON.stringify({ a: 1n }, (k, v) => (typeof v === "bigint" ? v.toString() : v)));
