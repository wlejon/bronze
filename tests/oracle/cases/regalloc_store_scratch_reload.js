// An array literal's elements are stored through its base, an index and a
// displacement, and on AArch64 an immediate (here the 0) goes to such an
// address through a scratch that a spilled value's reload also uses. The
// reload of the 3 for the second store once ran before the first store, which
// put its 0 there first: [0, 3] read back as 0, 0. Only inside an outer loop
// with the sum after it was the 3 spilled; the second half is where it was
// first seen (tiers_f64_demote_entry_params).
function opaque(x) { return Number(String(x)); }
function blur(src, dst, w, h) {
  for (let y = 1; y < h - 1; y++)
    for (let x = 1; x < w - 1; x++) {
      const i = y * w + x;
      dst[i] = (src[i - 1] + src[i + 1] + src[i - w] + src[i + w] + 4 * src[i]) * 0.125;
    }
}

const small = new Float64Array(4);
for (let round = 0; round < 1; round++) {
  const a = [0, 3];
  console.log(a[0], a[1]);
  let t = 0;
  for (let i = 0; i < 4; i++) t += small[i];
}

const src = new Float64Array(100).fill(1), dst = new Float64Array(100);
for (let round = 0; round < 2; round++) {
  const got = [];
  for (const [w, h] of [[8, 8], [7.5, 4], [-2, 3], [0, 3], [-0, 2], [3, 0], [NaN, 2]]) got.push(w + ":" + h);
  for (const [w, h] of [[10, 10], [10.5, 9], [-10, 9], [9.75, 10]]) {
    dst.fill(0);
    blur(src, dst, opaque(w), opaque(h));
    let t = 0;
    for (let i = 0; i < 100; i++) t += dst[i];
    got.push(t);
  }
  console.log(got.join(" "));
}
