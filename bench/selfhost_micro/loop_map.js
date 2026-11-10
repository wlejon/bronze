// A hand-written map: a new array filled by an index loop.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 20000; r++) {
    const b = new Array(a.length);
    for (let i = 0; i < a.length; i++) b[i] = a[i] * 2;
    sum += b[r % 1000];
}
console.log(sum);
