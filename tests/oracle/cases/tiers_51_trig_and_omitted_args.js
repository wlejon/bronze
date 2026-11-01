// Math.sin / Math.cos call-target speculation, including a builtin read into
// a local and called through it (`const cos = Math.cos; cos(x)`, three.js's
// setFromEuler), and method calls that omit trailing arguments, which the
// method IC's direct entry and the inline direct call extend with undefined
// (src/codegen-brass/il2mir/il_lowering_calls.cpp,
// src/runtime/rt_method_call.cpp). Each speculation then misses: another
// function under the same key, a non-Number argument. Every tier must print
// the same thing, with and without BRASS_DEOPT_STRESS.

function wave(t) {
  return Math.sin(t) * 0.5 + Math.cos(t * 2);
}
function viaLocal(obj, x) {
  const c = obj.cos;
  return c(x / 2);
}
function viaLocalSin(obj, x) {
  const s = obj.sin;
  return s(x);
}

let acc = 0;
for (let i = 0; i < 20000; i++) acc += wave(i * 0.001);
console.log(acc.toFixed(12));
const mine = { cos: (x) => 'mine' + x, sin: Math.sin };
let strs = '';
for (let i = 0; i < 20000; i++) {
  const r = viaLocal(i % 5000 === 7 ? mine : Math, i);
  if (typeof r === 'number') acc += r; else strs += r;
  const q = viaLocalSin(mine, i % 3 === 0 ? String(i) : i);
  acc += q;
}
console.log(acc.toFixed(12), strs);
console.log(Math.sin(-0), 1 / Math.sin(-0), Math.cos(NaN), Math.sin(Infinity), viaLocalSin(Math, undefined));
console.log(Math.sin(1e10), Math.cos(1e22), Math.sin(1e300), Math.cos(-823551), Math.cos(42.5));
console.log(viaLocal(Math, { valueOf() { return 1; } }), viaLocalSin(Math, null), viaLocalSin(Math, '0.5'));

class A {
  constructor() { this.n = 0; }
  f(x, y = 3) { this.n += x + y; return this; }
  g(x, y) { return y === undefined ? x : x * y; }
  h(x, y) { return arguments.length * 100 + (y === undefined ? 1 : 2) + x; }
  k(x, ...r) { return r.length + x; }
  m(a, b, c) { return typeof b + typeof c + a; }
}
const a = new A();
let s = 0, t = '';
for (let i = 0; i < 20000; i++) {
  a.f(i);
  if (i % 2) a.f(i, 1);
  s += a.g(i) + a.g(i, 2) + a.h(i) + a.h(i, 1) + a.k(i) + a.k(i, 1, 2);
  if (i % 5000 === 0) t += a.m(i) + a.m(i, 1) + ',';
}
console.log(a.n, s, t);
