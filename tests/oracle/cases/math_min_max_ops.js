// Math.max / Math.min with two to four arguments on the pristine builtin are
// math ops (il_op.h MathUnaryFn::Max / Min): every argument evaluated, then
// each converted with ToNumber in order, then folded with NaN winning and -0
// below +0. Other arities, spreads and a replaced Math stay calls.
function r(x) { return Object.is(x, -0) ? "-0" : String(x); }
const vals = [0, -0, 1, -1, 0.5, Infinity, -Infinity, NaN, 1e-310, -1e308];
const rows = [];
for (const a of vals) {
  const line = [];
  for (const b of vals) line.push(r(Math.max(a, b)) + "/" + r(Math.min(a, b)));
  rows.push(r(a) + ": " + line.join(" "));
}
console.log(rows.join("\n"));
// Three and four arguments, NaN anywhere, zeros of both signs.
console.log(r(Math.max(-0, 0, -0)), r(Math.min(0, -0, 0)), r(Math.max(1, NaN, 3)), r(Math.min(NaN, 1, 2, 3)));
console.log(r(Math.max(1, 2, 3, 4)), r(Math.min(4, 3, 2, 1)), r(Math.max(-0, -0, -0, -0)), r(Math.min(0, 0, 0, -0)));
// Values that are no Number: ToNumber of each, in order, after every argument
// is evaluated; a throwing valueOf stops the rest; BigInt throws.
const log = [];
const o = (n, v) => ({ valueOf() { log.push(n); return v; } });
const arg = (n, v) => { log.push("eval" + n); return o(n, v); };
console.log(r(Math.max(arg("a", 1), arg("b", 5))), r(Math.min(arg("c", 1), arg("d", NaN), arg("e", 2))), log.join(","));
log.length = 0;
try { Math.max(o("x", 1), { valueOf() { throw new Error("boom"); } }, o("z", 2)); } catch (e) { console.log(e.message, log.join(",")); }
try { Math.min(1, 2n); } catch (e) { console.log(e.constructor.name); }
console.log(r(Math.max("3", "10")), r(Math.min("3", "x")), r(Math.max(null, -1)), r(Math.min(undefined, 1)), r(Math.max(true, false)), r(Math.min([], [-2])));
// The clamp three.js calls on every Euler extraction.
function clamp(value, min, max) { return Math.max(min, Math.min(max, value)); }
let acc = 0;
for (let i = -50; i < 50; i++) acc += clamp(i / 10, -1, 1);
console.log(acc, r(clamp(-0, -1, 1)), r(clamp(NaN, -1, 1)), r(clamp("0.5", -1, 1)), r(clamp(2, -1, 1)));
// Other arities and forms stay calls.
console.log(Math.max(), Math.min(), r(Math.max(-0)), Math.max(...[1, 9, 3]), Math.min(1, 2, 3, 4, 5, 0));
const mx = Math.max;
console.log(mx(2, 7), [1, 5, 2].reduce((a, b) => Math.max(a, b)));
// Typed reads feeding them.
const f = new Float32Array([0.25, -3, 8]);
console.log(Math.max(f[0], f[1], f[2]), Math.min(f[0], f[1]), Math.max(f[5], 1));
