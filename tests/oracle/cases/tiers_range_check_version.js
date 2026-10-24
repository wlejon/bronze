// Loops over typed arrays whose sizes arrive at run time: the optimizer runs
// a copy without bounds checks when the first and last iteration are in
// bounds, the checked loop otherwise. Each kernel runs warm on sizes that fit,
// then on arrays too short for them, offsets that start out of bounds, and
// strides that walk off either end; out-of-bounds reads are undefined (NaN
// in arithmetic) and out-of-bounds writes are dropped, exactly as written.
function opaque(x) { return Number(String(x)); }

function blur(src, dst, w, h) {
  for (let y = 1; y < h - 1; y++)
    for (let x = 1; x < w - 1; x++) {
      const i = y * w + x;
      dst[i] = (src[i - 1] + src[i + 1] + src[i - w] + src[i + w] + 4 * src[i]) * 0.125;
    }
}

function mat(a, b, c, n) {
  for (let i = 0; i < n; i++)
    for (let j = 0; j < n; j++) {
      let s = 0;
      for (let k = 0; k < n; k++) s += a[i * n + k] * b[k * n + j];
      c[i * n + j] = s;
    }
}

function window(a, off, lo, hi) {
  let s = 0;
  for (let i = lo; i < hi; i++) s += a[i + off];
  return s;
}

function column(a, stride, col, n, v) {
  for (let k = 0; k < n; k++) a[k * stride + col] = v + k;
}

function digest(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s = (s * 31 + (a[i] === a[i] ? Math.round(a[i] * 1000) : 7)) % 1000000007;
  return s;
}

function fill(a, seed) {
  for (let i = 0; i < a.length; i++) a[i] = ((i * 2654435761 + seed) >>> 0) / 4294967296;
  return a;
}

const W = opaque(24), H = opaque(16);
const src = fill(new Float32Array(W * H), 1), dst = new Float32Array(W * H);
for (let k = 0; k < 400; k++) blur(src, dst, W, H);
console.log("blur fits", digest(dst));
const shortDst = new Float32Array(W * H - opaque(30));
blur(src, shortDst, W, H);
console.log("blur short dst", digest(shortDst));
const shortSrc = fill(new Float32Array(W * H - opaque(5)), 2), dst2 = new Float32Array(W * H);
blur(shortSrc, dst2, W, H);
console.log("blur short src", digest(dst2));
const dst3 = new Float32Array(W * H);
blur(src, dst3, opaque(25), H);
console.log("blur wide", digest(dst3));

const N = opaque(12);
const A = fill(new Float64Array(N * N), 3), B = fill(new Float64Array(N * N), 4), C = new Float64Array(N * N);
for (let k = 0; k < 300; k++) mat(A, B, C, N);
console.log("mat fits", digest(C));
const C2 = new Float64Array(N * N);
mat(A, B, C2, opaque(13));
console.log("mat n+1", digest(C2));
const Bshort = fill(new Float64Array(N * N - opaque(7)), 5), C3 = new Float64Array(N * N);
mat(A, Bshort, C3, N);
console.log("mat short b", digest(C3));

const V = fill(new Float64Array(opaque(50)), 6);
let ws = 0;
for (let k = 0; k < 2000; k++) ws += window(V, 3, 0, 40);
console.log("window fits", Math.round(ws * 1000));
for (const [off, lo, hi] of [[-1, 0, 10], [10, 0, 41], [0, -2, 5], [0, 45, 49], [0, 49, 51], [-60, 60, 70], [0, 5, 5], [0, 7, 3]]) {
  console.log("window", off, lo, hi, String(window(V, opaque(off), opaque(lo), opaque(hi))));
}

const D = new Float64Array(opaque(40));
for (let k = 0; k < 2000; k++) column(D, 4, 1, 9, k);
console.log("column fits", digest(D));
for (const [stride, col, n] of [[4, 1, 11], [-3, 20, 9], [0, 39, 3], [5, -2, 4], [7, 0, 6], [1, 38, 4]]) {
  const E = new Float64Array(opaque(40));
  column(E, opaque(stride), opaque(col), opaque(n), 100);
  console.log("column", stride, col, n, digest(E));
}
