// indexOf over 6 elements, hot loop.
const a = [3, 1, 4, 1, 5, 9];
let sum = 0;
for (let r = 0; r < 20000000; r++) sum += a.indexOf(r & 15);
console.log(sum);
