// 23.2.3's prototype methods over the two BIGINT views — the surface
// bigint64_array.js deliberately leaves out. The methods' loops move raw
// 64-bit payloads (a double cannot carry one), and a BigInt VALUE exists only
// where an element crosses into JS: a callback argument, `at`, an iterator
// result. What each block pins:
//
// - join/at/includes/indexOf/lastIndexOf: ToString of an element is decimal
//   digits with no `n`; the search needle is NEVER converted, so a Number
//   needle answers -1/false rather than matching or throwing — and a BigInt
//   needle above 2^64 answers by strict equality, not by a wrapping shortcut
//   (2^64 + 2 is congruent to 2 but is not 2n).
// - reverse/sort/fill/copyWithin/slice/with/toReversed/toSorted: the mutators
//   and the copying family, including 23.2.4.7's default sort order — the
//   SIGNED reading for BigInt64 (INT64_MIN first), the unsigned for BigUint64
//   — and a custom comparator receiving real BigInts.
// - fill(1)/with(0,1)/map(() => 1)/set([1]): every write converts through
//   7.1.13 ToBigInt, whose table has no Number row — each is a TypeError.
// - set: same content type moves as bytes even across the two kinds (the
//   unsigned maximum reads back as -1n through the signed view — the store
//   wraps modulo 2^64 and the bytes already are that answer); a Number view
//   as source is 23.2.5.1.17's TypeError, never a conversion.
// - callbacks (filter/map/reduce/every/some/find*/forEach): elements arrive
//   as BigInts — `typeof` says so — and filter/map rebuild views of the same
//   kind.
// - iterators (values/entries/keys, for-of over subarray): the same funnel.
// - the 13b/13d machinery holds: a stranded BigInt view's methods throw, and
//   a length-tracking BigInt view recomputes its window.
function kind(fn) {
  try { fn(); return "no throw"; } catch (e) {
    return e instanceof TypeError ? "TypeError" : e instanceof RangeError ? "RangeError" : "other";
  }
}

const a = new BigInt64Array([3n, -1n, 2n]);
console.log(a.join(","));
console.log(String(a.at(-1)), a.at(5));
console.log(a.includes(-1n), a.includes(7n), a.includes(2));
console.log(a.indexOf(2n), a.indexOf(2), a.lastIndexOf(-1n));
a.reverse();
console.log(a.join(","));
a.sort();
console.log(a.join(","));
a.sort((x, y) => (y > x ? 1 : y < x ? -1 : 0));
console.log(a.join(","));
a.fill(5n, 1);
console.log(a.join(","));
console.log(kind(() => a.fill(1)));

const s = a.slice(0, 2);
console.log(s.join(","), s.length);
const w = a.with(0, 7n);
console.log(w.join(","), a.join(","));
console.log(kind(() => a.with(0, 1)));
console.log(a.toReversed().join(","));
console.log(a.toSorted().join(","));

const t = new BigInt64Array(4);
t.set([1n, 2n]);
t.set(a.subarray(0, 1), 2);
console.log(t.join(","));
const uu = new BigUint64Array([18446744073709551615n]);
t.set(uu, 3);
console.log(String(t[3]));
console.log(kind(() => t.set(new Float64Array(2))));
console.log(kind(() => t.set([1])));

const c = new BigInt64Array([1n, 2n, 3n, 4n]);
console.log(c.filter((x) => x % 2n === 0n).join(","));
console.log(c.map((x) => x * 2n).join(","));
console.log(kind(() => c.map(() => 1)));
console.log(String(c.reduce((p, x) => p + x)));
console.log(String(c.reduce((p, x) => p + x, 100n)));
console.log(c.every((x) => x > 0n), c.some((x) => x > 3n));
console.log(String(c.find((x) => x > 2n)), c.findIndex((x) => x > 2n));
console.log(String(c.findLast((x) => x < 3n)), c.findLastIndex((x) => x < 3n));
let acc = 0n;
c.forEach((x, i) => { acc += x * BigInt(i); });
console.log(String(acc));
c.forEach((x, i) => { if (i === 0) console.log(typeof x); });

const it = c.values();
console.log(String(it.next().value), it.next().value === 2n);
const [k0, v0] = c.entries().next().value;
console.log(k0, String(v0));
console.log(c.keys().next().value);
for (const x of c.subarray(2)) console.log(String(x));

const cw = new BigInt64Array([1n, 2n, 3n, 4n]);
cw.copyWithin(0, 2);
console.log(cw.join(","));
console.log(String(c));
console.log(c.toString());

const sg = new BigInt64Array([1n, -2n, 0n, -9223372036854775808n]);
sg.sort();
console.log(sg.join(","));
const us = new BigUint64Array([18446744073709551615n, 0n, 5n]);
us.sort();
console.log(us.join(","));

console.log(c.indexOf(2n ** 64n + 2n));
console.log(c.includes(2n ** 64n + 2n));

const rb = new ArrayBuffer(16, { maxByteLength: 16 });
const rv = new BigInt64Array(rb, 8, 1);
rb.resize(0);
console.log(kind(() => rv.join(",")));

const rb2 = new ArrayBuffer(16, { maxByteLength: 32 });
const tv = new BigInt64Array(rb2);
tv[0] = 9n;
rb2.resize(32);
console.log(tv.length, String(tv[0]), tv.join(","));
