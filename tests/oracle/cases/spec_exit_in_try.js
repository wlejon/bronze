// Speculated code inside try, catch and finally: the first construction of
// each class below fails its guards (its fields' cells are not filled yet),
// so the first call leaves to the generic twin inside the try, the next one
// inside the catch and the finally. The twin's handlers must catch what the
// rest of the call throws, and its finally must run once.
class Step { constructor(v, i) { this.v = v; this.i = i; } }
class Miss { constructor(m, s) { this.m = m; this.s = s; } }
class Tail { constructor(s, n) { this.s = s; this.n = n; } }
function guarded(xs, limit, from) {
  let sum = 0.5, log = '', last = null, n = 0;
  for (let i = 0; i < xs.length; i++) {
    try {
      const v = xs[i] * 2;
      if (i >= from) last = new Step(v, i);
      if (v > limit) throw new Error('big ' + i);
      sum += v;
    } catch (e) {
      if (i >= from) last = new Miss(e.message, sum);
      log += e.message + ';';
      sum -= xs[i] * 0.5;
    } finally {
      if (i >= from && i % 4 === 3) last = new Tail(sum, n);
      sum += 0.25;
      n++;
    }
  }
  return sum + ' ' + n + ' ' + log + ' ' + JSON.stringify(last);
}
function outer(xs, limit, from) {
  try {
    return guarded(xs, limit, from);
  } catch (e) {
    return 'outer ' + (e && e.message);
  }
}
const xs = [];
for (let i = 0; i < 24; i++) xs.push(i * 1.5);
console.log(outer(xs, 30, 2));
console.log(outer(xs, 10, 9));
console.log(outer(xs, 20, 3));
let n = 0;
for (let r = 0; r < 3000; r++) n += outer(xs, 30, r % 24).length;
console.log('warm', n);
console.log(outer([1, 2, '3', 40, 5.5], 30, 0));
console.log(outer([1, { valueOf() { throw new Error('vo'); } }, 3], 30, 0));
console.log(outer([1, 2, NaN, undefined, 50, null], 30, 1));
