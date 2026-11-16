// The optimistic copies (opt/speculate optimistic.h), trained on one arm and
// run on the other (run.py's switch mode; the other modes run both arms).
// Training never takes the rare branches, never reads a third shape, never
// stores a non-Number: an optimistic copy assumes all three, leaves when the
// real run does them, and after enough such exits retires to its speculated
// copy, which handles them in place. `step` then gets receivers of another
// shape, so its speculated copy fails ordinary guards and retires to the
// generic function. Every call keeps its right answer throughout.
const TRAINING = Math.random() < 0.5;
class P { constructor(x) { this.x = x; this.y = 1; this.v = 0.5; } }
class Q { constructor(x) { this.y = 2; this.x = x; this.v = 0.25; } }
class R { constructor(x) { this.w = 0; this.x = x; this.y = 3; this.v = 0.125; } }
const extra = { n: 0, f(o) { this.n += o.y; return this.n & 7; } };

// A rare branch that calls out and rejoins.
function step(o, k) {
  let s = o.x + o.y;
  if (k > 1000) s += extra.f(o);
  for (let i = 0; i < 3; i++) s += (o.x * i) % 7 + o.y;
  return s + o.x * 2;
}
// Inlined into a caller with no assumption of its own.
function outer(o, k) { return step(o, k) + 1; }
// A read of two shapes, then a third.
function getX(o) { return o.x + o.v; }
// A store into a double slot, then of a string.
function setV(o, v) { o.v = v; return o; }
// The rare branch inside a loop.
function sumAll(arr, k) {
  let s = 0;
  for (let i = 0; i < arr.length; i++) {
    const o = arr[i];
    s += o.x;
    if (o.x > k) s += extra.f(o);
  }
  return s;
}

const ps = [], qs = [], rs = [];
for (let i = 0; i < 16; i++) { ps.push(new P(i)); qs.push(new Q(i + 1)); rs.push(new R(i + 2)); }

function train() {
  let s = 0;
  for (let r = 0; r < 20000; r++) {
    const p = ps[r & 15];
    s += step(p, r & 255) + outer(p, r & 127);
    s += getX(r & 1 ? p : qs[r & 15]);
    setV(qs[r & 15], (r & 7) + 0.5);
    s += sumAll(ps, 1000);
  }
  return s;
}

function real() {
  let s = 0;
  // The rare branches, on the trained shapes.
  for (let r = 0; r < 3000; r++) {
    const p = ps[r & 15];
    s += step(p, 5000) + outer(p, 2000);
    s += getX(r % 3 === 0 ? rs[r & 15] : p);
    setV(qs[r & 15], r & 1 ? 'v' : 1.5);
    s += sumAll(ps, 7);
  }
  // `step` on a shape its guards never saw.
  for (let r = 0; r < 3000; r++) s += step(rs[r & 15], r);
  return s + extra.n;
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(train());
} else {
  console.log(train());
  console.log(real());
  console.log(String(qs[3].v), extra.n);
}
