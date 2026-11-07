// A store to a typed array at a numeric key that is no valid index writes
// nothing, but TypedArraySetElement (10.4.5.16) converts the value first: a
// user valueOf runs (and its throw propagates) for every numeric key —
// negative, fractional, NaN, Infinity, past the end, "-0", "1.5" — while a key
// that is no canonical numeric string ("foo", "01") is an ordinary property.
const keys = [0, 1, "1", "-0", "1.5", "foo", "01", -1, 10, NaN, -0, 1.5, Infinity, "10", "-1", "Infinity", "NaN"];
for (const k of keys) {
  const counts = [];
  for (const C of [Float64Array, Float32Array, Int32Array, Uint8ClampedArray]) {
    let n = 0;
    const v = { valueOf() { n++; return 1; } };
    const a = new C(3);
    a[k] = v;
    counts.push(n + ":" + Array.from(a).join(",") + ":" + (Object.prototype.hasOwnProperty.call(a, k) ? "own" : "-"));
  }
  console.log(JSON.stringify(String(k)), counts.join(" "));
}
// Constant keys, sloppy and strict.
function constKeys(a, v) { a[-1] = v; a[1.5] = v; a["-0"] = v; a[NaN] = v; a[99] = v; }
function constKeysStrict(a, v) { "use strict"; a[-1] = v; a[1.5] = v; a["-0"] = v; a[NaN] = v; a[99] = v; }
for (const f of [constKeys, constKeysStrict]) {
  let n = 0;
  const v = { valueOf() { n++; return 2; } };
  const a = new Float32Array(2);
  for (let i = 0; i < 50; i++) f(a, v);
  console.log(f.name, n, Array.from(a).join(","));
}
// A throwing valueOf throws even when the store is discarded.
for (const k of [-1, 5, 0.5, "-0", 0]) {
  try {
    new Float64Array(2)[k] = { valueOf() { throw new RangeError("v" + String(k)); } };
    console.log("no throw", String(k));
  } catch (e) {
    console.log(e.constructor.name, e.message);
  }
}
// A BigInt view converts with ToBigInt: a Number throws, a BigInt is dropped.
for (const k of [-1, 5, "1.5", 0]) {
  const b = new BigInt64Array(2);
  try { b[k] = 3n; console.log("big ok", String(k), b.join(",")); } catch (e) { console.log("big", String(k), e.constructor.name); }
  try { b[k] = 3; console.log("num ok", String(k)); } catch (e) { console.log("num", String(k), e.constructor.name); }
}
// Strings and other primitives are converted too (no visible effect but NaN).
const s = new Float64Array(1);
s[-1] = "7"; s[0.5] = Symbol.iterator === undefined ? 1 : "8";
console.log(Array.from(s).join(","));
try { s[-1] = Symbol("x"); console.log("symbol stored"); } catch (e) { console.log("symbol", e.constructor.name); }
