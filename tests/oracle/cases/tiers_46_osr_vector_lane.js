// A biquad's loop, entered through OSR: the entry loads the frame's two
// unboxed coefficients as one f64x2 and takes each lane into an f64
// register. With the loop's slow paths calling out, the high lane's
// register was spilled, and brass's x64 selection of the lane (a copy of
// the whole vector into the f64 register, then a shufpd reading it back
// through an 8-byte reload) zeroed it: a2 became 0, the filter lost its
// damping and every call running the OSR code returned Infinity (tats
// tools/sfx/dsp.js grains, whose bandpass grains turned to NaN mid-batch).
// Every tier must print the same thing.
function filter(x, a1, a2) {
  const b1 = 0;
  let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (let i = 0; i < x.length; i++) {
    const v = x[i];
    const y = v + b1 * x1 - 0.5 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = v; y2 = y1; y1 = y;
    x[i] = y;
  }
  return x;
}

function peak(x) {
  let m = 0;
  for (let i = 0; i < x.length; i++) m = Math.max(m, Math.abs(x[i]));
  return m;
}

let bad = 0;
let differ = 0;
let first = -1;
let total = 0;
for (let it = 0; it < 120; it++) {
  const x = new Float32Array(300);
  for (let i = 0; i < x.length; i++) x[i] = ((i * 7919) % 101) / 5000 - 0.01;
  const m = peak(filter(x, -1.8, 0.9));
  if (!Number.isFinite(m)) bad++;
  if (first < 0) first = m;
  else if (m !== first) differ++;
  total += m;
}
console.log('peak ' + first.toFixed(9));
console.log('non-finite ' + bad + ' differing ' + differ);
console.log('total ' + total.toFixed(6));
