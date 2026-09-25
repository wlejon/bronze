// ToBoolean of a value the compiler has typed as a number: false at 0, -0 and
// NaN. The numeric test is the ORDERED "not 0" — the unordered compare `!==`
// uses answers true at NaN, which once made `+opts.x || 120` answer NaN.
// Every form that takes a condition is exercised: ||, &&, ?:, !, if, while,
// for and do-while, over NaN reached by unary +, 0/0, the literal and Math.
const d = {};
const n = NaN;
console.log(+d.x || 120, n || 1, !n, n ? 1 : 2, n && 1);
console.log((0 / 0) || "zero-over-zero", !(0 / 0), (0 / 0) ? "t" : "f");
console.log(Math.sqrt(-1) || "sqrt", +"abc" || "parse", Number(undefined) || "num");
console.log(-0 || "negzero", 0 || "zero", 5 || "five", -1 && "neg", Infinity && "inf");
console.log(!!n, !!(n * 2), !!(1 + 2), !!(1 - 1));

function tempo(opts) {
  return +opts.bpm || 120;
}
console.log(tempo({}), tempo({ bpm: "90" }), tempo({ bpm: 0 }), tempo({ bpm: "x" }));

function branch(x) {
  const v = x * 1;
  if (v) return "truthy";
  return "falsy";
}
console.log(branch(NaN), branch(0), branch(-0), branch(2), branch(-2), branch(1e-300));

let loops = 0;
let w = NaN;
while (w) { loops++; w = 0; }
for (let f = NaN; f; f = 0) loops++;
let dw = 1;
do { loops++; dw = dw / 0 - Infinity; } while (dw);
console.log(loops);

let acc = 0;
for (let i = 0; i < 6; i++) {
  const v = i % 3 === 0 ? NaN : i;
  acc += v ? v : 100;
}
console.log(acc);
