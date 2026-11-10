// Object literals stored into a long-lived array: the stores must still barrier.
const keep = new Array(4096);
for (let i = 0; i < 4096; i++) keep[i] = { v: 0 };
let sum = 0;
for (let i = 0; i < 10000000; i++) {
    const o = { v: i, w: i & 7 };
    keep[i & 4095] = o;
    sum += keep[(i * 7) & 4095].v;
}
console.log(sum);
