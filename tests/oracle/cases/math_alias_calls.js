// A module-scope `const cos = Math.cos` called with numbers is Math.cos
// itself (Math is never written here), lowered as the math op; a call with
// something not surely a number, and a shadowing local, keep their meaning.
const cos = Math.cos;
const sin = Math.sin;
const sqrt = Math.sqrt;
const floor = Math.floor;

function wave(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) acc += cos(i * 0.25) * sin(i * 0.5) + sqrt(i) - floor(i / 3);
  return acc;
}
console.log(wave(1000).toFixed(6));
console.log(cos("0"), sqrt("16"), floor({ valueOf() { return 2.7; } }));
console.log(cos(NaN), sqrt(-1), floor(-0) === 0 && 1 / floor(-0));

function shadow(x) {
  const sin = (v) => "local " + v;
  return sin(x);
}
console.log(shadow(1));

// The alias also escapes as a value here: the read stays.
const abs = Math.abs;
const fns = [abs, Math.trunc];
console.log(abs(-3), fns[0](-4), fns[0] === Math.abs, fns.map((f) => f(-2.5)).join(","));
