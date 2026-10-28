// Truthiness of values whose type the compiler cannot see: every kind of
// value through one untyped `if (v)`, `!v`, `v ? :` and `&&` site, warmed on
// numbers first so the inline number test is the hot path, then fed the
// edge cases (-0, NaN, denormals, int32 zero vs nonzero, 0n, "", holes).
function t(v) { if (v) return 1; return 0; }
function n(v) { return !v; }
function c(v) { return v ? "y" : "n"; }
function a(v, w) { return v && w ? "both" : "not"; }
for (let i = 0; i < 20000; i++) { t(i * 0.5); n(i); c(i - 3); a(i, i + 1); }
const big = 2 ** 31;
const vals = [
  0, -0, 1, -1, 0.5, -0.5, NaN, Infinity, -Infinity, 5e-324, -5e-324,
  Number.MAX_VALUE, Number.MIN_VALUE, big, -big, big - 1, (big - 1) | 0, 0 | 0, 7 | 0, -1 | 0,
  0n, 1n, -1n, 2n ** 70n,
  "", "0", " ", "false", "a",
  true, false, null, undefined,
  {}, [], [0], function () {}, Symbol("s"), new Number(0), new Boolean(false), new String(""),
  Math.sqrt(-1), 0 / -1, 1 / Infinity, -1 / Infinity,
];
const out = [];
for (const v of vals) out.push(`${t(v)}${n(v) ? 1 : 0}${c(v)}${a(v, v)}`.replace("both", "B").replace("not", "N"));
console.log(out.join(" "));
// Holes and missing properties read back as undefined.
const sparse = [1, , 0, , 2];
let s = "";
for (let i = 0; i < sparse.length + 2; i++) s += t(sparse[i]);
console.log(s);
const o = { x: 0, y: 1, z: "" };
console.log([o.x, o.y, o.z, o.w].map(t).join(""));
// Int32 arithmetic results that are zero, and values flipping kind at one site.
let acc = 0;
for (let i = -5; i <= 5; i++) { const v = (i * i - 4) | 0; acc = acc * 2 + t(v); }
console.log(acc);
const mix = [3, "x", 0, null, 1.5, "", -0, {}, NaN, 0n, true, undefined, false, 9n];
let m = "";
for (let r = 0; r < 3; r++) for (const v of mix) m += t(v);
console.log(m);
// Loop conditions and while(v) on an untyped value.
let k = 6, steps = 0;
let w = k;
while (w) { w = w > 1 ? w - 1 : (w === 1 ? "" : 0); steps++; }
console.log(steps, JSON.stringify(w));
