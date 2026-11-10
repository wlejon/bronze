// arr.map(x => x * 2) over 1000 elements, the result summed.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 20000; r++) {
    const b = a.map(x => x * 2);
    sum += b[r % 1000];
}
console.log(sum);
