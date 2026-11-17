// `%` on an inlined method's parameter the call site leaves out: the
// parameter is the constant undefined in the speculated caller, and the
// remainder's integer arm (il_lowering_numeric.cpp lower_dynamic_mod), never
// run for it, must still test the dividend's sign unsigned, as every compare
// of Value bits is (il_bridge_verifier.cpp refused the signed one).
class R {
  m(i) { return i % 4999; }
}
const o = new R();
function missing() { return o.m(); }
let n = 0;
for (let k = 0; k < 300000; k++) {
  if (Number.isNaN(missing())) n++;
}
console.log(n, o.m(5), o.m(10000), o.m(-3));
