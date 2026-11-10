// arr.map(x => x * 2) on 4-8 element arrays in a hot loop.
const arrs = [];
for (let n = 4; n <= 8; n++) {
    const a = [];
    for (let i = 0; i < n; i++) a.push(i + n);
    arrs.push(a);
}
let sum = 0;
for (let r = 0; r < 4000000; r++) {
    const b = arrs[r % 5].map(x => x * 2);
    sum += b[1];
}
console.log(sum);
