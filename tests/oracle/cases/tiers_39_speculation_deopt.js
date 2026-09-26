// Speculation sites (src/codegen-brass/il2mir/il_speculation.h): hot
// functions tier up with guards armed on the sites that stayed monomorphic
// and numeric, then see operands those guards refuse. Each refusal finishes
// the call in Tier 0, and repeated refusals invalidate the code and let it
// tier up again with the failing sites demoted. Every tier must print the
// same thing, with and without BRASS_DEOPT_STRESS.

// Number arithmetic that later sees strings.
function add(a, b) {
  return a + b;
}
function mix(a, b) {
  return (a - b) * (a + b) / 2;
}

// A property read and write that later see a second shape, a primitive and
// an accessor.
function readX(o) {
  return o.x;
}
function bump(o) {
  o.x = o.x + 1;
  return o.x;
}

// Overflow slots: the fifth property on lives outside the object.
function Wide(i) {
  this.a = i; this.b = i + 1; this.c = i + 2; this.d = i + 3;
  this.e = i + 4; this.f = i + 5; this.g = i + 6;
}
function wideSum(w) {
  w.f = w.f + w.a;
  return w.a + w.b + w.c + w.d + w.e + w.f + w.g;
}

let n = 0;
for (let i = 0; i < 20000; i++) n = add(n, i % 7);
console.log(n);
let s = '';
for (let i = 0; i < 12; i++) s = add(s, i);
console.log(s);
for (let i = 0; i < 20000; i++) n = add(n, 1);
console.log(n);

let m = 0;
for (let i = 0; i < 20000; i++) m += mix(i, 3);
console.log(m);
console.log(mix('8', '2'), mix(7n > 1n ? 5 : 0, 1), mix(null, undefined), mix([4], { valueOf() { return 2; } }));

const plain = { x: 1 };
let r = 0;
for (let i = 0; i < 20000; i++) r += readX(plain);
console.log(r);
const other = { y: 0, x: 40 };
const withGetter = { get x() { return 100; } };
console.log(readX(other), readX(withGetter), readX('str'), readX(5));
for (let i = 0; i < 30; i++) r += readX(i % 2 ? other : withGetter);
console.log(r);

const target = { x: 0 };
for (let i = 0; i < 20000; i++) bump(target);
console.log(target.x);
const frozen = Object.freeze({ x: 7 });
const setter = { log: [], set x(v) { this.log.push(v); }, get x() { return this.log.length; } };
console.log(bump(frozen), bump(setter), bump(setter), setter.log.join(','));
const holder = { x: 'a' };
console.log(bump(holder), bump(holder));
for (let i = 0; i < 20000; i++) bump(target);
console.log(target.x);

let ws = 0;
const ws1 = new Wide(1);
for (let i = 0; i < 20000; i++) ws += wideSum(ws1) % 1000;
console.log(ws, ws1.f);
const ws2 = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 'f', g: {} };
console.log(wideSum(ws2));
const ref = { k: 1 };
const ws3 = new Wide(0);
ws3.f = ref;
console.log(typeof wideSum(ws3), ws3.f);
