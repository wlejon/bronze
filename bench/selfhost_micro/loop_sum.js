// A hand-written index loop sum.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 100000; r++) {
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    sum += s + r;
}
console.log(sum);
