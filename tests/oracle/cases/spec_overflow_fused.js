// int32 add, subtract and multiply whose overflow check branches straight
// to the slow side, with the sum used after the check: counters and running
// sums that cross 2^31 (both ways) partway through a warmed-up run.
function sums(start, step, n) {
  let a = start, b = start, c = 1, d = 0;
  for (let i = 0; i < n; i++) {
    a = a + step;
    b = b - step;
    c = c * 3;
    if (c > 1e6 || c < -1e6) c = (c | 0) % 7 - 3;
    d = (d + i) - (i >> 1);
  }
  return [a, b, c, d];
}
function idx(n, k) {
  let t = 0;
  for (let i = 0; i < n; i++) {
    const j = i + k;
    t = t + j;
  }
  return t;
}
let w = 0;
for (let r = 0; r < 3000; r++) { const v = sums(r, 3, 40); w += v[0] - v[1] + v[2] + v[3] + idx(30, r); }
console.log('warm', w);
console.log(sums(2147483600, 7, 20).join());
console.log(sums(-2147483600, 9, 20).join());
console.log(sums(0, 2147483647, 3).join(), sums(-1, -2147483648, 2).join());
console.log(idx(5, 2147483645), idx(4, -2147483648), idx(3, 1073741824));
