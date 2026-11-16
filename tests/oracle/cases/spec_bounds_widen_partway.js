// Runs of element accesses at i, i+1, ... i+k: one bounds test is widened to
// cover the run. Partway through the last runs only a later access of a run
// falls out of bounds (an array shorter than the loop expects), with stores
// already made by earlier accesses of the same run; what the run leaves behind
// has to be what the separate accesses leave.
function pack(src, dst, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const j = i * 4;
    dst[j] = src[j] * 2;
    dst[j + 1] = src[j + 1] + 1;
    s += src[j + 2];
    dst[j + 3] = src[j + 3] - s;
  }
  return s;
}
function readMat(m, k) {
  const a = m[k], b = m[k + 1], c = m[k + 2], d = m[k + 5];
  return a * 1000 + b * 100 + c * 10 + (d === undefined ? -1 : d);
}
const src = new Float64Array(400);
for (let i = 0; i < 400; i++) src[i] = i % 17;
const dst = new Float64Array(400);
let t = 0;
for (let r = 0; r < 2000; r++) t += pack(src, dst, 100);
console.log('warm', t, dst[397], dst[399]);
let u = 0;
for (let r = 0; r < 4000; r++) u += readMat([1, 2, 3, 4, 5, 6, 7, 8], r & 1);
console.log('warmMat', u);
const shortDst = new Float64Array(398);
try { console.log('short dst', pack(src, shortDst, 100), shortDst[396], shortDst[397]); } catch (e) { console.log('err', e.message); }
const plainSrc = Array.from({ length: 401 }, (_, i) => i % 5);
const plainDst = [];
console.log('plain', pack(plainSrc, plainDst, 100), plainDst.length, plainDst[398], plainDst[399]);
const shortSrc = new Float64Array(399);
for (let i = 0; i < 399; i++) shortSrc[i] = 1;
console.log('short src', pack(shortSrc, dst, 100), dst[396], dst[397], dst[398], dst[399]);
console.log('mat', readMat([1, 2, 3, 4, 5, 6, 7, 8], 3), readMat([1, 2, 3, 4], 0), readMat([1, 2, 3, 4], 2), readMat([9, 8, 7], -1));
