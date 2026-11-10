// arr.find over 1000 elements.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 40000; r++) {
    const t = 500 + (r & 255);
    sum += a.find(x => x === t);
}
console.log(sum);
