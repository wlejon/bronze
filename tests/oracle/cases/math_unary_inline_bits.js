// Pristine Math.sin/cos/sqrt/abs/floor/ceil on a typed f64 are inlined
// (il_lowering_ops.cpp MathUnary): sqrt/abs/floor/ceil to the machine's exact
// instructions, sin/cos to the runtime's own kernels (fdlibm, bronze_math_*_f64).
// A built binary once called the C library's sin, whose last bit differs from
// node's on a few percent of inputs (Math.sin(2.5) printed ...565, not ...564).
// Each result is printed in full and compared to the same call reached through
// a computed key and through an alias, neither of which is inlined.

const inputs = [0, -0, 2.5, 1, -1, 0.5, 3.141592653589793, 1e-300, -1e-300,
  1e22, -1e22, 1e300, 2 ** 53, 2 ** 53 + 2, 99, 99.5, 1e6 + 0.25, 1e9,
  Infinity, -Infinity, NaN, 0.1, 0.7, 1.5707963267948966, 4.71238898038469,
  6.283185307179586, -2.5, 123456.789, 5e-324, 1.7976931348623157e308];

function fs(x) { return [Math.sin(x), Math.cos(x), Math.sqrt(x), Math.abs(x), Math.floor(x), Math.ceil(x)]; }
const names = ["sin", "cos", "sqrt", "abs", "floor", "ceil"];
const alias = { sin: Math.sin, cos: Math.cos };
let bad = 0;
for (const x of inputs) {
  const r = fs(+x);
  const line = r.map((v) => Object.is(v, -0) ? "-0" : String(v)).join(" ");
  console.log(String(x), line);
  for (let k = 0; k < names.length; k++) if (!Object.is(r[k], Math[names[k]](x))) { bad++; console.log("dyn differs", names[k], x); }
  if (!Object.is(r[0], alias.sin(x)) || !Object.is(r[1], alias.cos(x))) { bad++; console.log("alias differs", x); }
}

// A sweep: the sum of many sins and coss in one typed loop, and the count of
// inputs where the inlined path's bits differ from the dynamic call's (0).
let s = 0, c = 0, mism = 0;
const sinKey = "s" + "in";
for (let i = 0; i < 20000; i++) {
  const x = i * 0.37 - 3700;
  const a = Math.sin(x), b = Math.cos(x);
  s += a; c += b;
  if (!Object.is(a, Math[sinKey](x))) mism++;
}
console.log(s, c, mism, bad);

// Non-numbers keep their coercions: the inline needs a typed double.
console.log(Math.sin("0.5"), Math.cos(null), Math.sqrt([16]), Math.abs({ valueOf() { return -3; } }),
  Math.floor(undefined), Math.ceil("x"), Math.sqrt(-1), Math.sqrt(-0), Math.floor(-0.5), Math.ceil(-0.5));
