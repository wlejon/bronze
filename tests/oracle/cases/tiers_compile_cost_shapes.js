// The shapes the compile-cost work changed how brass handles, each checked
// for its answers in every tier: a long straight-line function with many
// calls and more live values than registers (the dead-register clearing at
// calls and the allocator's hints), loads made redundant across diamonds and
// loops with aliasing writes between them (GVN-PRE, whose dominator tree
// now outlives its iterations), dead induction variables in loops and none
// in straight-line code, and many functions hot at once (the compile queue's
// order).

function bump(o, k) { o.n = (o.n + k) | 0; return [o.n, k]; }

function wide(a, o) {
  const a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4], a5 = a[5], a6 = a[6], a7 = a[7];
  const a8 = a[8], a9 = a[9], a10 = a[10], a11 = a[11], a12 = a[12], a13 = a[13], a14 = a[14], a15 = a[15];
  const t0 = a0 * a5 - a1 * a4 + bump(o, 1)[0];
  const t1 = a2 * a7 - a3 * a6 + bump(o, 2)[1];
  const t2 = a8 * a13 - a9 * a12 + bump(o, 3).length;
  const t3 = a10 * a15 - a11 * a14 + bump(o, 4)[0];
  const keep = { x: t0, y: t1 };
  const t4 = a0 * a15 - a3 * a12 + bump(o, 5)[1];
  const t5 = a5 * a10 - a6 * a9 + bump(o, 6)[0];
  const t6 = (t0 + t1) * (t2 - t3) + bump(o, 7)[1];
  const t7 = (t4 - t5) / (1 + Math.abs(t6)) + bump(o, 8)[0];
  const det = t0 * t3 - t1 * t2 + t4 * t5 - t6 * t7;
  if (det === 0) return keep.x + keep.y;
  const inv = 1 / det;
  a[0] = t0 * inv; a[1] = t1 * inv; a[2] = t2 * inv; a[3] = t3 * inv;
  a[4] = t4 * inv; a[5] = t5 * inv; a[6] = t6 * inv; a[7] = t7 * inv;
  return keep.x - keep.y + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15;
}

function redundant(p, q, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    let v;
    if (i & 1) v = p.a * p.b + p.c;
    else { q.a = i; v = p.a * p.b; }
    s += v + p.a * p.b;
    if (i % 3 === 0) p.b = p.b + 1;
    s += p.a * p.b;
  }
  return s;
}

function dead_iv(n) {
  let live = 0, dead = 7, dead2 = 0;
  for (let i = 0; i < n; i++) { dead = dead + 3; dead2 = dead2 - i; live += i & 7; }
  let j = 0;
  while (j < n) j += 2;
  return live + j;
}

function straight(x) {
  const y = x > 0 ? x + 1 : x - 1;
  const z = y > 10 ? y * 2 : y * 3;
  return z - (x < 5 ? 1 : 2);
}

const hot = [];
function h0(x) { return (x * 3 + 0) % 1009; }
function h1(x) { return (x * 4 + 1) % 1009; }
function h2(x) { return (x * 5 + 2) % 1009; }
function h3(x) { return (x * 6 + 3) % 1009; }
function h4(x) { return (x * 7 + 4) % 1009; }
function h5(x) { return (x * 8 + 5) % 1009; }
function h6(x) { return (x * 9 + 6) % 1009; }
function h7(x) { return (x * 10 + 7) % 1009; }
function h8(x) { return (x * 11 + 8) % 1009; }
function h9(x) { return (x * 12 + 9) % 1009; }
function h10(x) { return (x * 13 + 10) % 1009; }
function h11(x) { return (x * 14 + 11) % 1009; }
hot.push(h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);

let total = 0;
const o = { n: 0 };
for (let r = 0; r < 4000; r++) {
  const a = [];
  for (let i = 0; i < 16; i++) a.push(((r + i * 7) % 13) - 6 + i / 4);
  total += wide(a, o);
  total += a[3];
}
console.log('wide', total.toFixed(6), o.n);

const p = { a: 3, b: 4, c: 5 };
console.log('redundant', redundant(p, p, 3000), redundant({ a: 1, b: 2, c: 3 }, { a: 0 }, 3000), p.a, p.b);

let d = 0;
for (let r = 0; r < 3000; r++) d += dead_iv(r % 50) + straight(r % 20 - 10);
console.log('dead_iv', d);

let h = 0;
for (let r = 0; r < 20000; r++) h = (h + hot[r % 12](r)) % 1000003;
console.log('hot', h);
