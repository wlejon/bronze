// A loop creating and dropping small arrays.
let sum = 0;
for (let i = 0; i < 10000000; i++) {
    const t = [i, i + 1, i & 3];
    sum += t[0] + t[2] - t[1];
}
console.log(sum);
