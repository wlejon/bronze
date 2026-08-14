// Direct dispatch of Math.sqrt/sin/cos/abs/min/max, and the guards that keep
// it honest. The hot loop uses only values whose answers ECMA-262 pins
// exactly (sqrt of a perfect square, abs/min/max of integers); sin and cos
// are implementation-approximated, so they are pinned by IDENTITY instead —
// the number-argument path and the string-argument path (which cannot take
// the fast dispatch) must produce the same bits, because both are defined to
// run the same function. Then the edge table 21.3.2 spells out — the empty
// call, the -0/+0 ordering, NaN anywhere — and finally an overwrite of
// Math.sqrt, which the code-pointer guard must notice on the next call.
let s = 0;
for (let i = 0; i < 500; i = i + 1) {
  s = s + Math.sqrt(i * i) + Math.abs(-i) + Math.min(i, i + 1) + Math.max(i, i - 1);
}
console.log(s);

let agree = true;
for (let i = 0; i < 100; i = i + 1) {
  const fast = Math.sin(i) + Math.cos(i);
  const slow = Math.sin("" + i) + Math.cos("" + i);
  if (fast !== slow) agree = false;
}
console.log(agree);

console.log(Math.sqrt(2.25));
console.log(Math.sqrt(-1));
console.log(Math.sin(0));
console.log(Math.cos(0));
console.log(Math.abs(-5.5));
console.log(Math.min());
console.log(Math.max());
console.log(1 / Math.min(0, -0));
console.log(1 / Math.max(-0, 0));
console.log(Math.min(NaN, 1));
console.log(Math.max(1, NaN));
console.log(Math.min(3, "2"));
console.log(Math.max("4", 1));

Math.sqrt = function (x) { return 100 + x; };
let t = 0;
for (let i = 0; i < 50; i = i + 1) t = t + Math.sqrt(i);
console.log(t);
