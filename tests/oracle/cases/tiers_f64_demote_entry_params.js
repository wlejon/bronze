// f64_demote takes a loop's f64 parameters for integers (and a loop limit's
// for sizes in [0, 2^26)), which only a check at entry makes true: a
// fractional, negative, -0, NaN or huge argument must run the code as
// written. Each function is called hot with integers first, so the demoted
// version is the one compiled, then with each awkward value. The sizes come
// through a string so no call site shows the compiler a constant.
function opaque(x) { return Number(String(x)); }

function blur(src, dst, w, h) {
  for (let y = 1; y < h - 1; y++)
    for (let x = 1; x < w - 1; x++) {
      const i = y * w + x;
      dst[i] = (src[i - 1] + src[i + 1] + src[i - w] + src[i + w] + 4 * src[i]) * 0.125;
    }
}
function remLoop(p) { let s = 0; for (let i = 0; i < 4; i++) s += p % 8; return s; }
function rowSum(a, w, h) {
  let s = 0;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const v = a[y * w + x]; s += v === undefined ? 1000 : v; }
  return s;
}
function stride(a, w) { let s = 0; for (let x = 0; x < 3; x++) { const v = a[x * w]; s += v === undefined ? 1000 : v; } return s; }
function offset(a, w, h) {
  let s = 0;
  for (let y = 1; y < h; y++) for (let x = 0; x < 3; x++) { const v = a[y * w + x + 20]; s += v === undefined ? 1000 : v; }
  return s;
}
function zeroSign(w) { let s = 0; for (let i = 0; i < 2; i++) s += 1 / (i * w); return s; }

const a = new Float64Array(64);
for (let i = 0; i < 64; i++) a[i] = i;
const src = new Float64Array(100).fill(1), dst = new Float64Array(100);

for (let round = 0; round < 3; round++) {
  const out = [];
  for (const p of [10.5, -3, 7, 1e300, -0, NaN, 2 ** 40 + 0.5]) out.push(remLoop(opaque(p)));
  for (const [w, h] of [[8, 8], [7.5, 4], [-2, 3], [0, 3], [-0, 2], [3, 0], [NaN, 2]]) {
    out.push(rowSum(a, opaque(w), opaque(h)));
    out.push(stride(a, opaque(w)));
    out.push(offset(a, opaque(w), opaque(h)));
  }
  for (const w of [1, -1, -0, 0, 0.5]) out.push(zeroSign(opaque(w)));
  for (const [w, h] of [[10, 10], [10.5, 9], [-10, 9], [9.75, 10]]) {
    dst.fill(0);
    blur(src, dst, opaque(w), opaque(h));
    let t = 0;
    for (let i = 0; i < 100; i++) t += dst[i];
    out.push(t);
  }
  if (round === 2) console.log(out.join(" "));
  for (let k = 0; k < 3000; k++) {
    remLoop(opaque(5));
    rowSum(a, opaque(8), opaque(8));
    stride(a, opaque(8));
    offset(a, opaque(-2), opaque(3));
    zeroSign(opaque(2));
    blur(src, dst, opaque(10), opaque(10));
  }
}
