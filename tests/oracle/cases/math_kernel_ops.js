// Math.atan2, exp, log, tan, asin, acos and atan on the PROVEN pristine
// builtin compiled to `math.unary` (lower_call.cpp): the runtime's own f64
// kernel called with machine numbers, no global read and no call. The edges
// IEEE pins (signed zeros, NaN, infinities, out-of-domain) print exactly; the
// rest to twelve places, since libm's last bit is the platform's. The two
// arguments of atan2 are both evaluated before either is converted, and the
// conversion runs y's valueOf before x's.

function r(x) { return Object.is(x, -0) ? "-0" : String(x); }
function f(x) { return Number.isFinite(x) && x !== 0 ? x.toFixed(12) : r(x); }

const zs = [0, -0, 1, -1, Infinity, -Infinity, NaN];
for (const y of zs) {
  const row = [];
  for (const x of zs) row.push(f(Math.atan2(y, x)));
  console.log(r(y), row.join(" "));
}

const edges = [0, -0, 0.5, -0.5, 1, -1, 2, -2, Infinity, -Infinity, NaN];
for (const x of edges) {
  console.log(r(x), f(Math.exp(x)), f(Math.log(x)), f(Math.tan(x)), f(Math.asin(x)),
              f(Math.acos(x)), f(Math.atan(x)));
}

// Values the inference cannot prove numbers: strings, booleans, null,
// undefined, objects with valueOf. The checked conversion is ToNumber.
const odd = ["0.5", "", "x", true, false, null, undefined, [], [2], { valueOf() { return -1; } }];
for (const v of odd) {
  console.log(f(Math.atan2(v, 1)), f(Math.atan2(1, v)), f(Math.asin(v)), f(Math.exp(v)));
}

// Order: both argument expressions, then y's conversion, then x's.
const log = [];
const y = { valueOf() { log.push("y.valueOf"); return 1; } };
const x = { valueOf() { log.push("x.valueOf"); return 1; } };
const res = Math.atan2((log.push("y expr"), y), (log.push("x expr"), x));
console.log(f(res), log.join(","));

// A throwing conversion of y leaves x unconverted.
const log2 = [];
try {
  Math.atan2({ valueOf() { log2.push("y"); throw new Error("boom"); } },
             { valueOf() { log2.push("x"); return 0; } });
} catch (e) {
  console.log("caught", e.message, log2.join(","));
}
// The natives with two or three arguments convert them left to right too.
const log3 = [];
const o = (n, v) => ({ valueOf() { log3.push(n); return v; } });
console.log(Math.pow(o("base", 2), o("exp", 3)), Math.hypot(o("a", 3), o("b", 4)),
            Math.hypot(o("c", 2), o("d", 3), o("e", 6)), Math["atan2"](o("y", 0), o("x", 1)), log3.join(","));
try {
  Math.atan2(1n, 1);
} catch (e) {
  console.log("bigint", e instanceof TypeError);
}

// Other arities are calls, and still answer as the builtin does.
console.log(r(Math.atan2(1)), r(Math.atan2()), f(Math.atan2(1, 1, 99)), r(Math.asin()), f(Math.atan(1, 2)));

// Through a const alias, and from typed and plain element reads.
const at2 = Math.atan2;
const as = Math.asin;
const ta = new Float32Array([0.25, -0.75, 0, -0]);
const pa = [0.25, -0.75, 0, -0, , "0.5"];
let s = 0;
for (let i = 0; i < 4000; i++) {
  const k = i & 3;
  s += at2(ta[k], pa[3 - k] + 1) + as(ta[k]) + Math.atan2(pa[k], ta[3 - k] - 2);
}
console.log(s.toFixed(8));
console.log(r(Math.atan2(pa[4], 1)), f(Math.atan2(pa[5], 1)), r(Math.atan2(ta[9], 1)), r(at2(-0, ta[2])));

// Euler-style extraction: asin of a clamped value and atan2 of products.
function extract(m11, m12, m13, m22, m23, m32, m33) {
  const yv = Math.asin(Math.max(-1, Math.min(1, m13)));
  if (Math.abs(m13) < 0.9999999) return [Math.atan2(-m23, m33), yv, Math.atan2(-m12, m11)];
  return [Math.atan2(m32, m22), yv, 0];
}
for (const m of [[1, 0, 0, 1, 0, 0, 1], [0, 0, 1, 1, 0, 0, 0], [0.5, -0.5, 0.7071, 0.8, 0.1, -0.2, 0.6]]) {
  console.log(extract(...m).map(f).join(" "));
}
