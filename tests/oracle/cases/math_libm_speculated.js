// Math.exp, log, tan, asin, acos, atan and atan2 at a call site the backend
// speculates on (il_lowering_calls.cpp: the function object's code word
// compared, every argument a Number, then the f64 kernel the builtin itself
// calls). The fast path and the builtin must agree, so each is called from a
// hot loop and from a computed-key read that never speculates; the edges
// (signed zeros, NaN, infinities, out-of-domain) print exactly, the sums to
// six or eight places, since libm's last bit is the platform's.

function r(x) { return Object.is(x, -0) ? "-0" : String(x); }

const edges = [0, -0, 1, -1, 0.5, 2, -2, Infinity, -Infinity, NaN];
for (const x of edges) {
  console.log(r(x), r(x === 0.5 ? Math.asin(x).toFixed(12) : Math.asin(x)), r(Math.acos(x) * 0 + (x === 1 ? 0 : 1)), r(Math.atan(x) === Math["atan"](x)),
              r(Math.exp(x) === Math["exp"](x)), r(Math.log(x)), r(Math.tan(x) === Math["tan"](x)));
}
const pairs = [[0, 0], [-0, 0], [0, -0], [-0, -0], [1, 0], [0, -1], [-0, -1], [Infinity, Infinity],
               [-Infinity, Infinity], [NaN, 1], [1, NaN], [3, 4]];
for (const [y, x] of pairs) console.log(r(y), r(x), r(Math.atan2(y, x) === Math["atan2"](y, x)), r(Math.atan2(y, x) > 0));

function kernelSum(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const t = (i % 200) / 100 - 1;
    s += Math.asin(t) + Math.acos(t) + Math.atan(t * 3) + Math.atan2(t, 1 - t) +
         Math.exp(t) + Math.log(2 + t) + Math.tan(t * 0.5);
  }
  return s;
}
function genericSum(n) {
  const M = Math;
  const names = ["asin", "acos", "atan", "atan2", "exp", "log", "tan"];
  let s = 0;
  for (let i = 0; i < n; i++) {
    const t = (i % 200) / 100 - 1;
    s += M[names[0]](t) + M[names[1]](t) + M[names[2]](t * 3) + M[names[3]](t, 1 - t) +
         M[names[4]](t) + M[names[5]](2 + t) + M[names[6]](t * 0.5);
  }
  return s;
}
console.log(kernelSum(5000).toFixed(6), kernelSum(5000) === genericSum(5000));

// Not Numbers: the site's slow path, ToNumber and all.
console.log(Math.asin("0.5") === Math.asin(0.5), Math.atan2("1", true), Math.exp(null), Math.log({ valueOf() { return 1; } }));
console.log(r(Math.atan2(1)), r(Math.asin()), Math.atan2.length, Math.asin.name, Math.atan2.name);

// A member replaced mid-run: the guard sees the new function object.
function twice(y, x) { return Math.atan2(y, x) + Math.asin(y / 4); }
let before = 0;
for (let i = 0; i < 100; i++) before += twice(1, 2);
const savedAtan2 = Math.atan2, savedAsin = Math.asin;
Math.atan2 = (y, x) => y * 100 + x;
Math.asin = () => 1000;
console.log(before.toFixed(8), twice(1, 2));
Math.atan2 = savedAtan2;
Math.asin = savedAsin;
console.log(twice(1, 2).toFixed(8));
