// 13.15.3 says an arithmetic operator needs its two operands to be the SAME
// numeric type, and 7.2.13/7.2.14 say the comparisons do not. Both halves are
// here, because the pair is the surprising part of the design: `1n + 1` is a
// TypeError while `1n == 1` is true.
function fail(f) {
  try { return String(f()); } catch (e) {
    return (e instanceof TypeError ? "TypeError" : "Error") + ": " + e.message;
  }
}
console.log(fail(() => 1n + 1));
console.log(fail(() => 1 + 1n));
console.log(fail(() => 1n - 1));
console.log(fail(() => 2n * 2));
console.log(fail(() => 4n / 2));
console.log(fail(() => 5n % 2));
console.log(fail(() => 2n ** 2));
console.log(fail(() => 1n & 1));
console.log(fail(() => 1n << 1));
console.log(fail(() => 1n + true));
console.log(fail(() => +1n));
console.log(fail(() => Math.abs(-1n)));
console.log(fail(() => Math.max(1n, 2n)));

// `+` reaches its numeric branch only after ToPrimitive, so a STRING operand
// concatenates and never mixes: 13.15.3 step 1.d wins before step 3.
console.log("" + 1n, 1n + "1", "x" + 2n + 3n, `${1n}${2n}`);

// 7.2.14 IsLooselyEqual compares BigInt against Number and String
// mathematically and EXACTLY — no conversion through a double.
console.log(10n == 10, 10n === 10, 10n != 10, 10n !== 10);
console.log(9007199254740993n == 9007199254740992, 9007199254740993n == 9007199254740993n);
console.log(1n == "1", 1n == " 1 ", 0n == "", 0n == "  ", 0n == "x", 1n == "0x1");
console.log(0n == null, 0n == undefined, 0n == false, 1n == true, 0n == 0, 1n == [1n]);

// 7.2.13 IsLessThan, same exactness. NaN makes the answer *undefined*, which
// all four operators report as false.
console.log(10n < 10.5, 10.5 < 11n, 9007199254740993n > 9007199254740992);
console.log(9007199254740993n < 9007199254740992, 1n < "2", "2" < 3n, "10" < 9n);
console.log(1n < NaN, NaN < 1n, 1n <= NaN, 1n >= NaN, 1n < Infinity, 1n > -Infinity);
console.log(2n > 1, 2 > 1n, 2n >= 2, 2n <= 2);

// SameValueZero (7.2.10) is what Set and Map key on, and it does not equate a
// BigInt with the Number of the same value.
const s = new Set([1n, 1, 2n, 2n]);
console.log(s.size, s.has(1n), s.has(1));
const m = new Map([[1n, "big"], [1, "num"]]);
console.log(m.size, m.get(1n), m.get(1));

// Array.prototype.sort's DEFAULT comparator is ToString, which every BigInt
// answers, so sorting a BigInt array needs no comparator and no mixing.
console.log([2n, 1n, 10n].sort(), [3, 1, 2].sort());
console.log([2n, 1n, 10n].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)));
