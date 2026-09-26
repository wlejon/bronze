// Speculation inside generator and async bodies (il2mir/il_speculation.h):
// each site kind, arithmetic, a property read and write, a Math call target
// and a polymorphic read, sits before and after a yield or an await. The
// bodies tier up on monomorphic numeric work, then meet operands their
// guards refuse, so a deopt finishes a resume in Tier 0 over the live frame
// and the next resume carries on from it. Every tier must print the same
// thing, with and without BRASS_DEOPT_STRESS.

function P(x, y) { this.x = x; this.y = y; }
function Q(y, x) { this.y = y; this.x = x; this.z = 0; }

// Arithmetic and a property read and write on both sides of a yield; the
// values that cross the yield (acc, i, o) are read by the sites after it.
function* stepper(o, n) {
  let acc = 0;
  for (let i = 0; i < n; i++) {
    acc = acc + o.x * 2 - i;
    o.x = o.x + 1;
    const sent = yield acc;
    acc = acc + sent * o.y - i / 2;
    o.y = o.y - 1;
  }
  return acc;
}

function driveStepper(o, n, sendOf) {
  const g = stepper(o, n);
  let r = g.next();
  let sum = 0;
  let k = 0;
  while (!r.done) {
    sum = sum + r.value;
    r = g.next(sendOf(k++));
  }
  return [sum, r.value, o.x, o.y];
}

const num = k => k % 5;
let out = [];
for (let rep = 0; rep < 60; rep++) out = driveStepper(new P(rep, 3), 200, num);
console.log(out.join(' '));
// Strings through the arithmetic after the yield, then before it.
console.log(driveStepper(new P(1, 2), 6, k => 'a' + k).join(' '));
console.log(driveStepper(new P('s', 2), 4, num).join(' '));
// A second shape and an accessor through the property sites.
console.log(driveStepper(new Q(3, 1), 20, num).join(' '));
let gets = 0;
const acc = { get x() { gets++; return 4; }, set x(v) { gets += v; }, y: 1 };
console.log(driveStepper(acc, 5, num).join(' '), gets);
for (let rep = 0; rep < 20; rep++) out = driveStepper(new P(rep, 3), 200, num);
console.log(out.join(' '));

// A Math call target before and after a yield, then a replaced Math.sqrt.
function* roots(n) {
  let s = 0;
  for (let i = 1; i <= n; i++) {
    s += Math.sqrt(i);
    const f = yield s;
    s += Math.floor(f * 1.5) + Math.abs(-i);
  }
  return s;
}
function sumRoots(n) {
  const g = roots(n);
  let r = g.next(0);
  let t = 0;
  let k = 0;
  while (!r.done) { t += r.value; r = g.next(k++); }
  return Math.round((t + r.value) * 1000) / 1000;
}
let rs = 0;
for (let rep = 0; rep < 60; rep++) rs = sumRoots(150);
console.log(rs);
const realSqrt = Math.sqrt;
Math.sqrt = x => x * 10;
console.log(sumRoots(10));
Math.sqrt = realSqrt;
console.log(sumRoots(150), sumRoots('3'));

// A polymorphic read (four shapes) on both sides of a yield, then eight.
function mk(k, v) {
  switch (k % 8) {
    case 0: return { v };
    case 1: return { a: 1, v };
    case 2: return { a: 1, b: 2, v };
    case 3: return { a: 1, b: 2, c: 3, v };
    case 4: return { b: 1, v };
    case 5: return { c: 1, v };
    case 6: return { d: 1, v };
    default: return { e: 1, v };
  }
}
function* polyGen(objs) {
  let t = 0;
  for (let i = 0; i < objs.length; i++) {
    t += objs[i].v;
    yield t;
    t += objs[objs.length - 1 - i].v * 2;
  }
  return t;
}
function polySum(objs) {
  let last = 0;
  for (const v of polyGen(objs)) last = v;
  return last;
}
const four = [];
for (let i = 0; i < 64; i++) four.push(mk(i % 4, i));
let ps = 0;
for (let rep = 0; rep < 80; rep++) ps = polySum(four);
console.log(ps);
const eight = [];
for (let i = 0; i < 64; i++) eight.push(mk(i, i));
console.log(polySum(eight), polySum(four));

// The same site kinds in an async function, before and after an await.
async function relax(o, n) {
  let e = 0;
  for (let i = 0; i < n; i++) {
    e = e + o.x * o.y + Math.sqrt(i);
    o.x = o.x + 0.5;
    const w = await i;
    e = e - w / 4 + o.y;
    o.y = o.y * 1;
  }
  return Math.round(e * 100) / 100;
}
(async () => {
  let r = 0;
  for (let rep = 0; rep < 60; rep++) r = await relax(new P(rep, 2), 40);
  console.log(r);
  console.log(await relax(new P('x', 2), 3));
  console.log(await relax(new Q(2, 1), 5));
  console.log(await relax({ x: 1, y: 2n > 1n ? 3 : 0, z: 9 }, 5));
  for (let rep = 0; rep < 20; rep++) r = await relax(new P(rep, 2), 40);
  console.log(r);
})();
