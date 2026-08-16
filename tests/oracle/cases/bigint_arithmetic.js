// 6.1.6.2's arithmetic operators. Every one is EXACT — no operand and no
// result passes through a double — and division truncates toward zero with
// the remainder taking the dividend's sign, which is C's rule and not the
// floor rule Python uses.
console.log(1n + 2n, 100n - 1n, 7n * 6n);
console.log(7n / 2n, (-7n) / 2n, 7n / -2n, (-7n) / -2n);
console.log(7n % 2n, (-7n) % 2n, 7n % -2n, (-7n) % -2n);
console.log(0n - 1n, -(1n), -(-1n), -(2n ** 64n));
console.log(-(-(2n ** 64n)) === 2n ** 64n);

// Carries and borrows across the limb boundary, which is where a bignum
// breaks if it breaks at all.
const limb = 4294967296n;
console.log(limb - 1n, limb, limb * limb, limb * limb - 1n);
console.log((limb ** 4n) / (limb ** 2n), (limb ** 4n + 12345n) % (limb ** 2n));

// The identity a = (a/b)*b + a%b, over every sign pairing.
const pairs = [[17n, 5n], [-17n, 5n], [17n, -5n], [-17n, -5n],
               [1n, 100000000000000000000n], [-1n, 100000000000000000000n]];
for (const [x, y] of pairs) {
  console.log(x / y * y + x % y === x, x / y, x % y);
}

// 30!, which is 33 digits and beyond every fixed-width integer.
let f = 1n;
for (let i = 1n; i <= 30n; i++) f = f * i;
console.log(f);

// `++` and `--` add ONE OF THE OPERAND'S OWN TYPE (13.4.4.1 step 3).
let c = 9007199254740993n;
c++; console.log(c, typeof c);
c--; c--; console.log(c);
let n = 5; n++; console.log(n, typeof n);

function fail(f) {
  try { return String(f()); } catch (e) {
    const kind = e instanceof RangeError ? "RangeError" : e instanceof TypeError ? "TypeError" : "Error";
    return kind + ": " + e.message;
  }
}
console.log(fail(() => 1n / 0n));
console.log(fail(() => 1n % 0n));
console.log(fail(() => 2n ** -1n));
console.log(fail(() => 2n ** 3n));
