// A receiver tested once and read again (il_speculation.h, "PROVEN FACTS"):
// the later reads of one value skip the object and header tests where an
// earlier read's dominates them, in the speculated function and in whatever
// part of the generic copy and the twin has that dominance. Each function
// here reads a receiver several times with calls, arithmetic and joins
// between, runs long on plain objects, and then meets receivers that are
// not plain (an Array, a function, a string, a number, a Proxy, null) and
// variables that change which value they hold between two reads.

function bump(b) {
  b.x += b.vx;
  b.y += b.vy;
  if (b.x > 100) b.vx = -b.vx;
  if (b.y > 100) b.vy = -b.vy;
  return b.x + b.y;
}

function noisy(o, f) {
  let s = o.a;
  s += f(o);
  s += o.b;
  if (s > 10) s -= o.a;
  else s += o.b;
  return s + o.a + o.b;
}

function swap(o, p, n) {
  let cur = o;
  let s = 0;
  for (let i = 0; i < n; i++) {
    s += cur.a;
    if (i & 1) cur = p;
    else cur = o;
    s += cur.b;
  }
  return s;
}

function store(o, v) {
  o.a = v;
  o.b = o.a + 1;
  o.a = o.b + o.a;
  return o.a;
}

function nested(o) {
  const p = o.inner;
  let s = p.a;
  o.count = o.count + 1;
  s += p.b + o.count;
  return s + o.inner.a;
}

function keyed(o, k) {
  let s = o.a;
  s += o[k];
  s += o.b;
  return s;
}

const tap = (o) => o.b;
let total = 0;
const b = { x: 0, y: 0, vx: 1.5, vy: 2.5 };
const o = { a: 1, b: 2 };
const p = { a: 10, b: 20 };
const n = { inner: { a: 3, b: 4 }, count: 0 };
for (let i = 0; i < 20000; i++) {
  total += bump(b);
  total += noisy(o, tap);
  total += swap(o, p, 4);
  total += store({ a: 0, b: 0 }, i);
  total += nested(n);
  total += keyed(o, 'b');
}
console.log(total);

function show(f) {
  try {
    const r = f();
    console.log(typeof r, String(r));
  } catch (e) {
    console.log('throws', e.constructor.name);
  }
}

const arr = [1, 2, 3];
arr.a = 5; arr.b = 6; arr.x = 1; arr.y = 2; arr.vx = 3; arr.vy = 4;
const fn = function () {};
fn.a = 7; fn.b = 8; fn.x = 200; fn.y = 300; fn.vx = 1; fn.vy = 1;
const log = [];
const prox = new Proxy({ a: 1, b: 2, x: 1, y: 2, vx: 3, vy: 4, inner: { a: 1, b: 2 }, count: 0 }, {
  get(t, k) { log.push('get ' + String(k)); return t[k]; },
  set(t, k, v) { log.push('set ' + String(k)); t[k] = v; return true; },
});
const acc = { get a() { log.push('a'); return 2; }, set a(v) { log.push('a=' + v); }, b: 3 };
const receivers = [arr, fn, 'str', 42, prox, acc, null, undefined, Object.create(o), new (class { a = 4; b = 5; })()];
for (const r of receivers) {
  show(() => bump(r));
  show(() => noisy(r, tap));
  show(() => noisy(o, () => r === null ? 0 : 1));
  show(() => swap(o, r, 6));
  show(() => swap(r, o, 6));
  show(() => store(r, 3));
  show(() => nested({ inner: r, count: 1 }));
  show(() => nested(r));
  show(() => keyed(r, 'b'));
  show(() => keyed(o, 'a'));
}
console.log(log.join(','));

// The plain receivers again, behind the exits: the functions answer as
// before whichever copy their callers now reach.
for (let i = 0; i < 2000; i++) {
  total += bump(b) + noisy(o, tap) + swap(o, p, 4) + store({ a: 0, b: 0 }, i) + nested(n) + keyed(o, 'b');
  if (i % 100 === 0) total += swap(arr, o, 3) + keyed(fn, 'a');
}
console.log(total);
