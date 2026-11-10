// forEach with a closure capturing and writing an outer variable.
const a = [];
for (let i = 0; i < 1000; i++) a.push(i);
let sum = 0;
for (let r = 0; r < 40000; r++) {
    let acc = r;
    a.forEach(x => { acc += x; });
    sum += acc;
}
console.log(sum);
