// A hot loop runs in OSR code that carries its callee's body, and the callee
// then meets a second object shape. The failures invalidate the OSR code;
// a frame still looping in it must go back to the interpreter at its next
// backedge rather than fail the carried copy's guard on every iteration
// (about 20,000 deopts in one frame before brass's OSR leave check).
function f(o) {
  let s = 0;
  for (let i = 0; i < 20; i++) s += o.a * o.b + i;
  return s;
}
const plain = { a: 3, b: 2 };
function cb(n, o) {
  let s = 0;
  for (let i = 0; i < n; i++) s += f(plain);
  return s + f(o);
}
let total = 0;
for (let frame = 0; frame < 40; frame++) {
  const odd = frame >= 8 && frame % 2;
  const o = odd ? { ['q' + frame]: 1, a: 3, b: 2 } : plain;
  total += cb(frame < 8 ? 300000 : 20000, o);
}
console.log(total);
