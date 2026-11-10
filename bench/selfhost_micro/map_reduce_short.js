// The result of a short map, immediately reduced: the array never escapes.
const a = [1, 2, 3, 4, 5, 6];
let sum = 0;
for (let r = 0; r < 4000000; r++) {
    sum += a.map(x => x * r).reduce((s, x) => s + x, 0);
}
console.log(sum);
