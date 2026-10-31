// oracle: Math.random drawn, no drawn value printed
// A program that assigns Math.random: every call sees the assignment (the
// direct form of math_random_direct.js must not be taken here).
const original = Math.random;
let n = 0;
function draw() { return Math.random(); }
console.log('before', typeof draw(), draw() < 1);
Math.random = function () { n++; return 0.25; };
console.log('patched', draw(), draw(), Math.random(), n);
Math.random = original;
const x = draw();
console.log('restored', x >= 0 && x < 1, n);
