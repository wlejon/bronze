// Element reads guarded against holes and then used as Numbers, where the
// hole guard becomes the Number guard (redundancy/strengthen.cpp), and the
// same reads meeting a hole, a string, an object with a valueOf, a read past
// the end and undefined in the last rounds: products and valueOf calls must
// be node's.
"use strict";
function mul(a, b, out) {
  const a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
  const b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
  out[0] = a0 * b0 + a1 * b2;
  out[1] = a0 * b1 + a1 * b3;
  out[2] = a2 * b0 + a3 * b2;
  out[3] = a2 * b1 + a3 * b3;
  return out;
}
function main() {
  const b = [5, 6, 7, 8];
  let total = 0, calls = 0;
  const v = { valueOf() { calls++; return 2; } };
  const odd = [
    [1, , 3, 4],
    [1, "2", 3, 4],
    [1, 2, v, 4],
    [1, 2, 3],
    [undefined, 2, 3, 4],
  ];
  const a = [1, 2, 3, 4];
  for (let i = 0; i < 20010; i++) {
    const late = i >= 20000;
    const x = late ? odd[(i - 20000) % odd.length] : a;
    if (!late) a[i & 3] = (i % 7) + 1;
    const out = [0, 0, 0, 0];
    mul(i < 20005 ? x : b, i < 20005 ? b : x, out);
    if (late) console.log(out.join(","));
    else total += out[0] + out[1] + out[2] + out[3];
  }
  console.log(total, calls);
}
main();
