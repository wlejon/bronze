// Math.max / Math.min with two to four Number arguments run inline
// (codegen-brass il_lowering_calls.cpp, BuiltinOp::Max/Min) behind a guard on
// the callee and the arguments' kinds. Every answer below must be the
// variadic builtin's: NaN anywhere, -0 below +0, and the full call for
// anything that is not a Number or not the builtin.

function show(v) { return Object.is(v, -0) ? '-0' : String(v); }
function max2(a, b) { return Math.max(a, b); }
function min2(a, b) { return Math.min(a, b); }
function max3(a, b, c) { return Math.max(a, b, c); }
function min4(a, b, c, d) { return Math.min(a, b, c, d); }

const vals = [0, -0, 1, -1, 2.5, NaN, Infinity, -Infinity, 1e-320, -1e-320];
const out = [];
for (let r = 0; r < 3; r++) {
  out.length = 0;
  for (const a of vals) for (const b of vals) out.push(show(max2(a, b)) + '/' + show(min2(a, b)));
}
console.log(out.join(' '));

const triples = [[0, -0, -0], [-0, -0, 0], [-0, -0, -0], [1, NaN, 2], [3, 2, 1], [-1, -2, -0], [0, 0, 0]];
for (const [a, b, c] of triples) console.log(show(max3(a, b, c)), show(min4(a, b, c, a)), show(min4(c, b, a, -0)));

// Non-Number arguments: the full builtin, ToNumber on every argument in order.
const log = [];
const v1 = { valueOf() { log.push('v1'); return 5; } };
const v2 = { valueOf() { log.push('v2'); return NaN; } };
console.log(max2(v1, 3), min2('7', 2), max3(v1, v2, 1), log.join(','));
console.log(max2(true, null), min2(undefined, 1), max2('x', 1));
try { max2(1n, 2); } catch (e) { console.log(e instanceof TypeError); }

// Aliases.
const mx = Math.max;
let s = 0;
for (let i = 0; i < 100; i++) s += mx(i & 3, 2);
console.log(s);
console.log(max2(1, 2), Math.max(), Math.min(), Math.max(3), Math.max(1, 2, 3, 4, 5));
