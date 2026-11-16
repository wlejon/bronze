// TRAINING true runs the warm-up alone: a harvest of that run, fed to a
// build of this file, makes every switched call below fail a guard.
const TRAINING = false;
// Numeric ops trained on int32 operands, then given fractions, -0, NaN,
// overflow, strings, objects and BigInts.
function arith(a, b) { return [a + b, a - b, a * b, a / b, a % b, -a]; }
function bits(a, b) { return [a & b, a | b, a ^ b, ~a, a << b, a >> b, a >>> b]; }
function cmp(a, b) { return [a < b, a <= b, a > b, a >= b, a == b, a === b]; }
function count(n) { let c = 0; for (let i = 0; i < n; i++) c += i; return c; }
function countDown(n) { let c = 0; for (let i = n; i > 0; --i) c -= i; return c; }
function hash(s, x) { return (s ^ (x & 255)) * 16777619 >>> 0; }
let acc = 0;
for (let i = 0; i < 30000; i++) {
  const a = (i % 97) + 1, b = (i % 13) + 1;
  const r = arith(a, b), q = bits(a, b & 31), c = cmp(a, b);
  acc = (acc + r[0] + r[1] + r[2] + (r[4] | 0) - r[5] + q[0] + q[1] + q[2] + q[3] + q[4] + q[5] + q[6] + (c[0] ? 1 : 0)) | 0;
  acc = hash(acc, i) | 0;
}
console.log(acc, count(1000), countDown(1000));
const show = (v) => Array.isArray(v) ? v.map(show).join(' ') : Object.is(v, -0) ? '-0' : typeof v === 'bigint' ? v + 'n' : String(v);
if (!TRAINING) {
  const cases = [[0.5, 1.25], [-0, 1], [0, -0], [1, 0], [-1, 0], [0, 0], [NaN, 3], [2147483647, 1], [-2147483648, 1],
    [-2147483648, -1], [65536, 65536], [1e300, 1e300], [Infinity, 1], ['7', 2], [7, '2'], ['a', 'b'], [{ valueOf() { return 5; } }, 3],
    [null, 4], [undefined, 4], [true, 2], [5n, 3n], [-7, 3], [7, -3], [-7.5, 2], [4294967295, 1], [2.9, 33], [-1, 31]];
  for (const [a, b] of cases) {
    let line = [];
    for (const f of [arith, bits, cmp]) {
      try { line.push(show(f(a, b))); } catch (e) { line.push(e.constructor.name); }
    }
    console.log(line.join(' | '));
  }
  console.log(count(2.5), count('3'), countDown(3.5), count(2147483647 - 5 > 0 ? 3 : 0));
  console.log(hash(1.5, 2.5), hash(-1, 300), hash('x', 1), hash(2 ** 40, 7));
  let s = 2147483640; for (let i = 0; i < 20; i++) s = s + 1; console.log(s);
  let t = 0; for (let i = 0; i < 10; i += 0.5) t += i; console.log(t);
}
