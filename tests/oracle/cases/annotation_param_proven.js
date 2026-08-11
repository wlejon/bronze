// docs/0010 decision 6: an annotation that inference independently proves is
// free information. `scale` is direct-callable — its name is only ever the
// callee of a call — and every call site passes a number, so the parameters
// really are f64 and the multiply is a native one. The `: number` on them is
// not why; it agrees with the proof and is otherwise ignored.
//
// The values below are IEEE-754 doubles either way, which is what makes this
// case worth running: if the typed path ever stopped being ordinary double
// arithmetic, the third line would move.
function scale(x: number, k: number): number {
  return x * k;
}

console.log(scale(6, 7));
console.log(scale(1.5, 2));
// 0.1 is the double 3602879701896397 / 2^55; times 3 that is exactly halfway
// between two doubles, and round-half-to-even lands above 0.3 (ECMA-262
// 6.1.6.1.5 multiply, 6.1.6.1.20 Number::toString).
console.log(scale(0.1, 3));
