// A loop in OSR code calls functions too big to inline. The optimized copy of
// the loop links such a callee through its stub instead of carrying and
// re-optimizing its body, so the callee's own tier changes under the loop:
// it tiers up mid-loop, its shape guards fail and it deopts, and it is
// compared by identity. Each of those must leave the loop's results as Tier 0
// computes them. Every tier must print the same thing.
function big(o, i) {
  // Enough straight-line work that no inliner limit takes it.
  let x = o.a | 0, y = o.b | 0, z = (o.a + o.b) | 0;
  x = (x * 31 + y) | 0; y = (y ^ (x >>> 3)) | 0; z = (z + (x & y)) | 0;
  x = (x + (z << 2)) | 0; y = (y * 17 - z) | 0; z = (z ^ (y >>> 5)) | 0;
  x = (x ^ (i & 255)) | 0; y = (y + (x >>> 7)) | 0; z = (z * 13 + x) | 0;
  x = (x * 7 + (z & 1023)) | 0; y = (y ^ (z << 3)) | 0; z = (z - (y >> 2)) | 0;
  x = (x + (y & z)) | 0; y = (y * 29 + (x >>> 11)) | 0; z = (z ^ (x * 3)) | 0;
  x = (x ^ (y >>> 13)) | 0; y = (y + (z & 4095)) | 0; z = (z * 5 - x) | 0;
  x = (x * 11 + (y >> 4)) | 0; y = (y ^ (x << 1)) | 0; z = (z + (y >>> 9)) | 0;
  x = (x - (z & 255)) | 0; y = (y * 3 + (z >>> 2)) | 0; z = (z ^ (x + y)) | 0;
  x = (x + (y ^ z)) | 0; y = (y + (x & 63)) | 0; z = (z * 19 + (y >> 6)) | 0;
  x = (x ^ (z >>> 4)) | 0; y = (y - (x << 2)) | 0; z = (z + (x ^ y)) | 0;
  x = (x * 23 + (y & z)) | 0; y = (y ^ (z >>> 8)) | 0; z = (z - (x >> 3)) | 0;
  x = (x + (z * 9)) | 0; y = (y * 37 + (x >>> 6)) | 0; z = (z ^ (y + 1)) | 0;
  let t = 0;
  for (let k = 0; k < 3; k++) t = (t + ((x >> k) ^ (y << k) ^ (z >>> k))) | 0;
  if (o.c !== undefined) t = (t + o.c * 1000) | 0;
  return (x ^ y ^ z ^ t) | 0;
}
function bigF(o, i) {
  let s = o.a * 0.5 + o.b * 0.25;
  s = s * 1.0001 + i; s = s / 1.5 + o.a; s = s * 0.999 - o.b; s = s + Math.sqrt(i + 1);
  s = s * 1.0002 - i * 0.5; s = s / 1.25 + o.b * 2; s = s - o.a / 3; s = s + Math.sqrt(s * s + 1);
  s = s * 0.75 + i / 7; s = s - Math.floor(s / 1000) * 1000; s = s * 1.5 + o.a * o.b;
  s = s / 2.5 + (i & 15); s = s * 1.125 - (i >> 3); s = s + o.b / 9; s = s - Math.floor(s / 997) * 997;
  s = s * 1.01 + 3; s = s / 1.7 - o.a; s = s + i * 0.125; s = s - Math.floor(s / 991) * 991;
  return s;
}

const plain = { a: 3, b: 2 };
const other = { c: 4, a: 5, b: 7 };
const objs = [plain, other];

// The callee is cold when the loop's entry is compiled, and turns hot inside it.
function coldThenHot(n) {
  let h = 0;
  let f = 0;
  for (let i = 0; i < n; i++) {
    h = (h + big(plain, i)) | 0;
    f += bigF(plain, i);
  }
  return h + ' ' + f.toFixed(3);
}

// The callee meets a second shape partway: it deopts under the loop.
function flips(n, at) {
  let h = 0;
  for (let i = 0; i < n; i++) h = (h ^ big(i >= at ? other : plain, i)) | 0;
  return h;
}

// The loop picks the callee and compares it by identity each iteration.
function pick(n) {
  let h = 0;
  let same = 0;
  for (let i = 0; i < n; i++) {
    const fn = (i & 3) === 0 ? bigF : big;
    if (fn === big) same++;
    const v = fn(objs[i & 1], i);
    h = (h + (typeof v === 'number' && v === (v | 0) ? v : Math.floor(v))) | 0;
  }
  return h + ' ' + same;
}

console.log(coldThenHot(50000));
console.log(flips(80000, 60000));
console.log(flips(80000, 0));
console.log(pick(60000));
console.log(coldThenHot(3));
console.log(flips(5, 2));
