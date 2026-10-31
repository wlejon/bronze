// Math.sin replaced by the program: a sine/cosine pair over one value must call
// the replacement, not the fused kernel, and the builtin again once restored.
// (Its own case: a program that assigns Math.sin compiles every Math.sin as a
// call of whatever the property holds.)
function pair(x) {
  const v = +x;
  const c = Math.cos(v);
  const s = Math.sin(v);
  return s + ' ' + c;
}
function halfAngles(x) {
  const cos = Math.cos;
  const sin = Math.sin;
  const v = +x;
  return sin(v / 2) + ' ' + cos(v / 2);
}
console.log('builtin', pair(0.5), halfAngles(1));
const realSin = Math.sin;
const realCos = Math.cos;
let seen = 0;
Math.sin = function (x) { seen++; return 42 + x; };
console.log('patched sin', pair(0.5), halfAngles(1), seen);
Math.cos = function (x) { seen++; return -x; };
console.log('patched both', pair(0.5), halfAngles(1), seen);
Math.sin = realSin;
console.log('cos only', pair(0.5), halfAngles(1), seen);
Math.cos = realCos;
console.log('restored', pair(0.5), halfAngles(1), seen);
for (let i = 0; i < 5; i++) {
  if (i === 3) Math.cos = function (x) { return 7; };
  console.log('loop', i, pair(i * 0.7));
}
Math.cos = realCos;
