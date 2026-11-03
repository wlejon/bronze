// A named member of the global object (`globalThis.BENCH_FLAG`) leaves Math
// the builtin, so the aliases below lower as the math op; `globalThis.Math =`
// rebinds the bare name, and every alias taken after it is the replacement.
globalThis.BENCH_FLAG = 3;
function half(x) {
  const cos = Math.cos, sin = Math.sin;
  return sin(x / 2).toFixed(6) + ' ' + cos(x / 2).toFixed(6);
}
console.log('builtin', half(1), globalThis.BENCH_FLAG);
const real = Math;
globalThis.Math = { sin: (x) => ({ toFixed: () => 'sin' + x }), cos: (x) => ({ toFixed: () => 'cos' + x }) };
console.log('rebound', half(1));
globalThis.Math = real;
console.log('restored', half(1));
