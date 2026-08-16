// 6.1.6.2's bitwise operators, which are defined over an INFINITE two's
// complement representation: a negative BigInt has infinitely many leading
// one bits, so `~0n` is -1n and `-1n >> 100n` is still -1n. Nothing here is
// ToInt32'd, which is the whole difference from the Number operators.
console.log(~0n, ~1n, ~-1n, ~255n);
console.log(5n & 3n, 5n | 3n, 5n ^ 3n);
console.log(-5n & 3n, -5n | 3n, -5n ^ 3n);
console.log(-5n & -3n, -5n | -3n, -5n ^ -3n);

// The shifts. `>>` is an ARITHMETIC shift and floors, so -1n stays -1n and
// -5n >> 1n is -3n rather than -2n.
console.log(1n << 64n, 1n << 0n, 5n >> 1n, -5n >> 1n, -1n >> 100n);
console.log(-1n << 1n, 1n << 100n >> 100n);
// A negative shift count reverses the direction (6.1.6.2.9 is one operation).
console.log(1n << -1n, 8n >> -2n);

// The limb boundary, where a 32-bit-limbed bignum breaks if it breaks.
console.log(1n << 32n, -1n << 32n, -4294967297n >> 32n, (1n << 96n) >> 64n);
console.log((1n << 64n) & (2n ** 64n - 1n), (2n ** 64n - 1n) ^ (2n ** 64n - 1n));
console.log((2n ** 64n - 1n) & -1n, (2n ** 64n - 1n) | -1n);

// 21.2.2.1 and 21.2.2.2: the modulo-2^bits window, read back signed and
// unsigned.
console.log(BigInt.asIntN(8, 255n), BigInt.asUintN(8, -1n));
console.log(BigInt.asIntN(8, 127n), BigInt.asIntN(8, 128n), BigInt.asIntN(8, -129n));
console.log(BigInt.asIntN(64, 2n ** 63n), BigInt.asUintN(64, -1n));
console.log(BigInt.asIntN(0, 5n), BigInt.asUintN(0, -5n));
console.log(BigInt.asIntN(32, 4294967295n), BigInt.asUintN(32, -1n));

// 6.1.6.2.11: there is no unsigned right shift on a BigInt, because an
// unsigned shift needs a width and a BigInt has none.
function fail(f) {
  try { return String(f()); } catch (e) {
    return (e instanceof TypeError ? "TypeError" : "Error") + ": " + e.message;
  }
}
console.log(fail(() => 1n >>> 1n));
console.log(fail(() => -1n >>> 0n));
