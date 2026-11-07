// A loop-free body over views it is handed (lower/typed_body_version.h): the
// copy entered when every receiver is a Float32Array or a Float64Array
// flags its element reads and writes, and must answer exactly what the
// generic path answers — for both kinds mixed in one call, out-of-bounds,
// fractional, negative, -0 and NaN indices, non-Number values written,
// Float32 rounding — while any other receiver (a plain array with holes, an
// Int32Array, an object) takes the original body.

function r(x) { return Object.is(x, -0) ? "-0" : typeof x === "number" ? String(x) : typeof x + ":" + String(x); }

// slerpFlat's shape: reads behind numbers, compares of the read values
// (`!==` sees undefined and NaN apart), a pure math call, writes.
function mix(dst, dstOffset, src0, srcOffset0, src1, srcOffset1, t) {
  let x0 = src0[srcOffset0 + 0], y0 = src0[srcOffset0 + 1];
  const x1 = src1[srcOffset1 + 0], y1 = src1[srcOffset1 + 1];
  if (t === 0) {
    dst[dstOffset + 0] = x0;
    dst[dstOffset + 1] = y0;
    return "t0";
  }
  if (x0 !== x1 || y0 !== y1) {
    const len = Math.atan2(y1 - y0, x1 - x0);
    x0 = x0 * (1 - t) + x1 * t + len * 0;
    y0 = y0 * (1 - t) + y1 * t;
  }
  dst[dstOffset] = x0;
  dst[dstOffset + 1] = y0;
  return x0 === x0 ? "num" : "nan";
}

function show(a) { return Array.prototype.map.call(a, r).join(","); }

const f32 = new Float32Array([0.1, 0.2, 0.3, 0.4]);
const f64 = new Float64Array([0.1, 0.2, 0.3, 0.4, 0, 0]);
for (let i = 0; i < 200; i++) mix(f64, 4, f32, 0, f32, 2, (i % 10) / 10);
console.log(show(f64));
const d32 = new Float32Array(4);
for (let i = 0; i < 200; i++) mix(d32, 0, f64, 0, f64, 2, 0.25);
console.log(show(d32));
// Same buffer as source and destination, the mixer's _slerp call.
const buf = new Float64Array([1, 2, 3, 4, 5, 6]);
mix(buf, 0, buf, 0, buf, 2, 0.5);
console.log(show(buf));

// Out of bounds: a read is undefined, so `!==` and the arithmetic see it;
// a write past the end is dropped.
const short = new Float32Array([1, 2, 3]);
console.log(mix(short, 2, short, 0, short, 2, 0.5), show(short));
console.log(mix(new Float64Array(2), 0, short, 2, short, 1, 0), show(short));
const out = new Float64Array(3);
console.log(mix(out, 2, f64, 0, f64, 1, 0.5), show(out));
// Fractional, negative, -0 and NaN offsets.
const g = new Float64Array([7, 8, 9]);
console.log(mix(g, -0, g, 0.5, g, 1, 0.5), show(g));
console.log(mix(g, -1, g, 0, g, 1, 0.5), show(g));
console.log(mix(g, NaN, g, 0, g, 1, 0.5), show(g));
console.log(mix(new Float32Array(4), 1.5, f32, 0, f32, 2, 0.5));

// Values that are no Number written into a view: ToNumber, with valueOf
// run once per store, in order.
const seen = [];
const vals = [{ valueOf() { seen.push("a"); return 3; } }, "4.5", true, null, undefined, [], [7], "x"];
function put(dst, k, a, b, c, d) {
  dst[k] = a; dst[k + 1] = b; dst[k + 2] = c; dst[k + 3] = d;
  return dst[k] + dst[k + 1];
}
const p32 = new Float32Array(8);
console.log(put(p32, 0, vals[0], vals[1], vals[2], vals[3]), put(p32, 4, vals[4], vals[5], vals[6], vals[7]), show(p32), seen.join(""));
const p64 = new Float64Array(5);
console.log(put(p64, 1, 1 / 3, 1e300, -0, NaN), show(p64));
console.log(put(p32, 0, 1 / 3, 1e300, -0, NaN), show(p32));

// Other receivers take the original body: plain arrays (holes, strings),
// an Int32Array, a Uint8ClampedArray, an object with index keys.
const plain = [0.5, , "2", 3];
const pdst = [];
console.log(mix(pdst, 0, plain, 0, plain, 2, 0.5), show(pdst));
console.log(mix(pdst, 0, plain, 0, plain, 1, 0), show(pdst));
const i32 = new Int32Array([1, 2, 3, 4]);
console.log(mix(i32, 0, i32, 0, i32, 2, 0.5), show(i32));
const u8 = new Uint8ClampedArray([10, 20, 300, 40]);
console.log(mix(u8, 0, f32, 0, f32, 2, 0.5), show(u8));
const obj = { 0: 1, 1: 2, 2: 3, 3: 4 };
console.log(mix(obj, 0, obj, 0, obj, 2, 0.5), JSON.stringify(obj));
// A view mixed with a plain array in one call.
const mixed = new Float64Array(2);
console.log(mix(mixed, 0, f32, 0, [9, 9], 0, 0.5), show(mixed));
// Properties beside the elements: a named key on a view is no element.
const named = new Float32Array(2);
named.foo = 5;
console.log(put(named, 0, 1, 2, 3, 4), show(named), named.foo, named[2], named[3]);
// Subarrays read and write through their own offset into the buffer.
const whole = new Float64Array([1, 2, 3, 4, 5, 6, 7, 8]);
const sub = whole.subarray(2, 6);
console.log(mix(sub, 0, sub, 0, sub, 2, 0.5), show(whole));

// Keys that are no Number reach the flagged accesses too: a string that is a
// canonical numeric index ("1", "-0" is not one, "1.5" is one that misses),
// a plain name, an object whose toString runs; and values of every kind.
// (Parameters and calls, not an object's fields and an array literal: those
// are field copies, which the pass leaves to the backend's element runs; and
// one read by a Number index, which makes `a` a receiver the pass versions.)
function rk(a, k, v, n) {
  a[k] = v;
  a[k] = v;
  return r(a[k]) + "/" + r(a[k]) + "/" + r(a[k]) + "/" + r(a[k]) + "|" + r(a[n | 0]);
}
function rkStrict(a, k, v, n) {
  "use strict";
  a[k] = v;
  a[k] = v;
  return r(a[k]) + "/" + r(a[k]) + "/" + r(a[k]) + "/" + r(a[k]) + "|" + r(a[n | 0]);
}
const calls = [];
const keyObj = { toString() { calls.push("k"); return "1"; } };
const valObj = { valueOf() { calls.push("v"); return 6.5; } };
const keys = [0, 1, "1", "-0", "1.5", "foo", -1, 10, NaN, -0, keyObj, 1.5, Infinity];
const values = [2, "3", valObj, undefined, 0.1];
for (const f of [rk, rkStrict]) {
  for (const k of keys) {
    const row = [];
    for (const v of values) {
      const a32 = new Float32Array(3), a64 = new Float64Array(3);
      let out;
      try {
        out = f(a32, k, v, 1) + " " + f(a64, k, v, 2) + " " + show(a32) + " " + show(a64) + " " + a32[k];
      } catch (e) {
        out = e.constructor.name;
      }
      row.push(out);
    }
    console.log(f.name, r(k), row.join(" | "));
  }
}
console.log(calls.join(""));
