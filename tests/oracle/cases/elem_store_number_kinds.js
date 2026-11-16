// A store of a known Number keeps an Array's NUMBERS bit, so in a speculated
// build the store does not write the `ElemKinds` location and a NUMBERS dense
// test of another (or the same) array stays answered across it (il/effects.h).
// Trained on Numbers; late rounds store a non-Number through every writer
// that must still kill the fact: an alias of the read array, push, a
// call, and a store of a value only sometimes a Number.
function add(a, p, w, n) {
    for (let i = 0; i < n; i++) w[i] = a[i] + p[i];
    return w[n - 1];
}
function copyMaybe(a, w, n, odd) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        w[i] = (i === odd) ? "s" + i : a[i];
        s += typeof a[i] === "number" ? a[i] : 1000;
    }
    return s;
}
function pushAndRead(a, w, n, v) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        w.push(i === n - 1 ? v : i);
        s += a[i];
    }
    return s;
}
function callBetween(a, w, n, f) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        w[i] = a[i] * 2;
        f(a, i);
        s += a[i];
    }
    return s;
}
const N = 16;
function fresh(k) { const r = []; for (let i = 0; i < N; i++) r.push(i + k); return r; }
let out = [];
for (let round = 0; round < 4000; round++) {
    const late = round > 3900;
    const a = fresh(round % 5), p = fresh(1), w = fresh(2);
    // aliasing: w is a when late, and a non-Number goes in through w
    const target = late && round % 3 === 0 ? a : w;
    let r = add(a, p, target, N);
    r += copyMaybe(a, late ? a : w, N, late ? 3 : -1);
    r += pushAndRead(a, late ? a : w, N, late ? "x" : 7);
    r += callBetween(a, w, N, late ? (arr, i) => { if (i === 5) arr[6] = "y"; } : () => {});
    if (round % 400 === 0 || late) out.push(String(r));
}
console.log(out.join(","));
