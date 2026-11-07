// Adjacent element loads used only as scalars stay scalar loads; each still
// reads the value its own read in program order sees (a store between two
// adjacent reads, through the same array or an alias, splits them), and
// pairs that do feed vector arithmetic keep working.
function dot4(f, g) {
  const a0 = f[0], a1 = f[1], a2 = f[2], a3 = f[3];
  const b0 = g[0], b1 = g[1], b2 = g[2], b3 = g[3];
  return a0 * b3 - a1 * b2 + a2 * b1 - a3 * b0;
}
function readWriteRead(f, h) {
  const x = f[0];
  h[1] = x + 100;      // h may be f
  const y = f[1];
  f[2] = y * 2;
  const z = f[2], w = f[3];
  return [x, y, z, w];
}
function lanes(f) {
  // pairs that feed lane-wise arithmetic
  const s0 = f[0] + f[4], s1 = f[1] + f[5], s2 = f[2] + f[6], s3 = f[3] + f[7];
  f[8] = s0; f[9] = s1; f[10] = s2; f[11] = s3;
  return s0 * 1000 + s1 * 100 + s2 * 10 + s3;
}
class V { constructor(x, y, z, w) { this.x = x; this.y = y; this.z = z; this.w = w; } }
function sumV(v) { const x = v.x, y = v.y, z = v.z, w = v.w; return ((x - y) * z) / (w || 1); }

const f64 = new Float64Array([1.5, -2, 3.25, 4, 5, 6, 7, 8, 0, 0, 0, 0]);
const f32 = new Float32Array([1.5, -2, 3.25, 4, 5, 6, 7, 8, 0, 0, 0, 0]);
const arr = [1.5, -2, 3.25, 4, 5, 6, 7, 8, 0, 0, 0, 0];
let acc = 0;
for (let i = 0; i < 30000; i++) {
  f64[i & 7] += 0.125; arr[(i + 3) & 7] -= 0.0625;
  acc += dot4(f64, arr) + dot4(f32, f64) + sumV(new V(i, 2, 3, i & 3));
}
console.log('acc', acc.toFixed(6));
for (const t of [new Float64Array([1, 2, 3, 4]), [1, 2, 3, 4], new Float32Array([1, 2, 3, 4])]) {
  console.log('rwr-self', readWriteRead(t, t).join(','), Array.from(t).join(','));
}
const src = new Float64Array([1, 2, 3, 4]);
console.log('rwr-alias', readWriteRead(src, new Float64Array(src.buffer)).join(','), Array.from(src).join(','));
console.log('rwr-other', readWriteRead(new Float64Array([1, 2, 3, 4]), new Float64Array(4)).join(','));
console.log('lanes', lanes(f64), lanes(arr), Array.from(f64.subarray(8)).join(','), arr.slice(8).join(','));
