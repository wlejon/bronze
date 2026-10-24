// A typed-array read whose value is used as it is (compared with undefined,
// returned, stored in a binding that is) gives undefined for an index past
// the view, not the NaN the numeric fast path stands in with. Each function
// sees one kind of view, so the compiler knows the element type, and runs hot
// first, so the compiled tiers are the ones answering.
function opaque(x) { return Number(String(x)); }
function isUndef(a, i) { return a[i] === undefined; }
function bound(a, i) { const v = a[i]; return v === undefined; }
function ret(a, i) { const v = a[i]; return v; }
function orDefault(a, i) { const v = a[i]; return v === undefined ? 1000 : v; }
function kind(a, i) { return typeof a[i]; }
function inLoop(a, w) {
  let s = 0;
  for (let x = 0; x < 1; x++) { const v = a[x + w]; s += v === undefined ? 1000 : v; }
  return s;
}
function isUndef32(a, i) { return a[i] === undefined; }
function orDefault8(a, i) { const v = a[i]; return v === undefined ? 1000 : v; }

const f64 = new Float64Array(4), f32 = new Float32Array(4), u8 = new Uint8Array(4);
f64.fill(2); f32.fill(3); u8.fill(4);
for (let k = 0; k < 3000; k++) {
  isUndef(f64, k & 1 ? 100 : 2); bound(f64, 1); ret(f64, 1); orDefault(f64, 2); kind(f64, 3); inLoop(f64, 1);
  isUndef32(f32, 2); orDefault8(u8, 2);
}
const out = [];
for (const n of ["0", "3", "4", "100", "-1", "1.5", "NaN"]) {
  const i = opaque(n);
  out.push(isUndef(f64, i), bound(f64, i), ret(f64, i), orDefault(f64, i), kind(f64, i));
  out.push(isUndef32(f32, i), orDefault8(u8, i));
}
out.push(inLoop(f64, 100), inLoop(f64, opaque(3)), f64[100] === undefined);
console.log(out.join(" "));
