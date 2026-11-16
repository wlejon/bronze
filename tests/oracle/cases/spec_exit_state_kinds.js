// A speculated function's failed guards hand its generic twin values it
// holds unboxed: doubles (NaN, -0 and infinities among them), int32s,
// booleans, and the tagged values beside them. The first time a literal's
// shape or a constructor's fields are made, their guards fail (the cells the
// harvest named are not filled yet), so the first call of each function
// below leaves to its twin deep in nested loops, in either arm of a branch,
// with every kind of value live across the exit; the answers must be node's.
class Rec {
  constructor(f, n, b) { this.f = f; this.n = n; this.b = b; }
}
function mix(xs, k, flag) {
  let f = 0.5, i32 = 0, b = false, neg = -0, inf = -Infinity;
  const out = [];
  for (let i = 0; i < xs.length; i++) {
    const x = xs[i];
    f = f * 0.75 + x;
    i32 = (i32 + i * k) | 0;
    b = !b && i % 3 === 0;
    neg = neg * 0.5;
    inf = inf / 2;
    for (let j = 0; j < 3; j++) {
      f += xs[(i + j) % xs.length] * 0.125;
      i32 = (i32 ^ j) | 0;
      if (j === 2 && i % 7 === k % 7) {
        if (flag) out.push({ f, i32, b, neg, inf });
        else out.push(new Rec(f, i32, b));
      }
    }
  }
  return JSON.stringify([f, i32, b, Object.is(neg, -0), inf, out.length, out[0]]);
}
function pick(a, b, c) {
  const s = a + b;
  const p = a * c;
  const n = (a | 0) + (c | 0);
  const t = a < b;
  if (t) return [{ s, p, n, t }, s].length + s + p + n;
  return [{ p, s, t, n }, p].length + s - p - n;
}
const nums = [];
for (let i = 0; i < 64; i++) nums.push(i * 0.25 - 3);
const odd = [NaN, -0, 2 ** 31, -(2 ** 31) - 1, Infinity, 5.5, 1e300];
console.log(mix(nums, 3, 1));
console.log(mix(nums, 5, 0));
console.log(mix(odd, 2, 1), mix(odd, 1, 0));
console.log(pick(1.5, 2.5, 3), pick(2.5, 1.5, -0), pick(NaN, 1, 2 ** 31));
let acc = 0;
for (let r = 0; r < 4000; r++) {
  acc += mix(nums, r & 7, r & 1).length;
  acc += pick(r * 0.5, r + 0.25, 3.5);
}
console.log('warm', acc);
console.log(mix(odd, 6, 1), mix(odd, 4, 0), pick(-0, 0, -0));
