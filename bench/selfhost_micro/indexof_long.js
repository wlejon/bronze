// indexOf over 1000 elements.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 100000; r++) sum += a.indexOf(500 + (r & 255));
console.log(sum);
