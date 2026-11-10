// arr.filter over 1000 elements.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 20000; r++) {
    const m = r & 7;
    const b = a.filter(x => (x & 7) === m);
    sum += b.length + b[3];
}
console.log(sum);
