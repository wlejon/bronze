// forEach with a capturing closure over a 6-element array, hot loop.
const a = [1, 2, 3, 4, 5, 6];
let sum = 0;
for (let r = 0; r < 4000000; r++) {
    let acc = 0;
    a.forEach(x => { acc += x * r; });
    sum += acc;
}
console.log(sum);
