// env.get.tdz of a binding fixed once initialized (an outer `const`, a `let`
// nothing reassigns) is speculated as env.get behind an is.uninit guard
// (opt/speculate/tdz.cpp). The hot loops read such bindings; some calls come
// before the binding is initialized and must still throw, after which the
// same functions run on the initialized value.
function early(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += LATE.x * i + (lateLet | 0);
  return s;
}
function tryEarly(n) {
  try { return early(n); } catch (e) { return e.constructor.name; }
}
const results = [];
results.push(tryEarly(3));
const K = { x: 2, y: 3 };
let scale = 1.5;
function hot(points) {
  let acc = 0;
  for (let i = 0; i < points.length; i++) {
    const p = points[i];
    acc += p.x * K.x + p.y * K.y + scale;
  }
  return acc;
}
const pts = [];
for (let i = 0; i < 64; i++) pts.push({ x: i, y: i * 0.5 });
let total = 0;
for (let r = 0; r < 4000; r++) total += hot(pts);
results.push(total);
// A generator whose block re-runs makes a fresh binding each time; each
// closure keeps the one it was made in.
function* makers() {
  for (let k = 0; k < 3; k++) {
    const C = { x: k + 1 };
    yield (n) => { let s = 0; for (let i = 0; i < n; i++) s += C.x; return s; };
  }
}
for (const f of makers()) { let t = 0; for (let r = 0; r < 3000; r++) t += f(10); results.push(t); }
const LATE = { x: 4 };
let lateLet = 7;
let e = 0;
for (let r = 0; r < 3000; r++) e += early(8);
results.push(e, tryEarly(2));
// A closure made before its binding's initializer runs, called on both sides.
function outer() {
  const before = [];
  const read = (n) => { let s = 0; for (let i = 0; i < n; i++) s += V * i; return s; };
  try { before.push(read(2)); } catch (err) { before.push(err.constructor.name); }
  const V = 3;
  let t = 0;
  for (let r = 0; r < 2000; r++) t += read(5);
  before.push(t);
  return before;
}
for (let r = 0; r < 3; r++) results.push(outer().join(' '));
console.log(results.join('\n'));
