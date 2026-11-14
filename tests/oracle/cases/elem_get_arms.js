// A computed read and a `length` read each meet one receiver kind in the
// warm-up, then every other: the read's arms (an Array, a view and its
// element kinds, a keyed slot) and the length's (a String, an Array, a view,
// a plain object) each have to answer for a receiver the warm-up never saw.
function at(list, i) {
    return list[i];
}
function len(x) {
    return x.length;
}
function sum(list) {
    let t = 0;
    for (let i = 0; i < len(list); i++) t += at(list, i);
    return t;
}
const kinds = [Float64Array, Float32Array, Int32Array, Uint32Array, Int16Array, Uint16Array, Int8Array, Uint8Array,
               Uint8ClampedArray];
const views = kinds.map((K, n) => {
    const v = new K(16);
    for (let i = 0; i < v.length; i++) v[i] = (i - 5) * (n + 1.5) * 1000003;
    return v;
});
let total = 0;
// One kind each, long enough to be the harvest's only one.
for (const order of [[0], [1], [2, 3], [4, 5, 6, 7, 8]]) {
    for (let r = 0; r < 400; r++) for (const k of order) total += sum(views[k]);
    // Then the rest.
    for (let k = 0; k < views.length; k++) total += sum(views[k]);
    total += sum([1, 2, 3, , 5]) || 0;
    total += len('four') + len({ length: 7 }) + (len({}) === undefined ? 1 : 0) + len(new Array(3));
    total += at({ 2: 40, x: 1 }, 2) + at('abc', 1).length + (at(views[0], 99) === undefined ? 1 : 0);
}
console.log(total);

function arrOnly(list) {
    let t = 0;
    for (let i = 0; i < list.length; i++) t += list[i];
    return t;
}
let a = 0;
const plainList = [];
for (let i = 0; i < 100; i++) plainList.push(i);
for (let r = 0; r < 300; r++) a += arrOnly(plainList);
a += arrOnly(views[1]) + arrOnly('12') .length + arrOnly({ length: 2, 0: 5, 1: 6 });
const big = new BigInt64Array(2);
big[0] = 5n;
a += Number(at(big, 0)) + len(big);
console.log(a);

function strOnly(s) {
    let t = 0;
    for (let i = 0; i < s.length; i++) t += s.charCodeAt(i);
    return t;
}
let c = 0;
for (let r = 0; r < 300; r++) c += strOnly('hello world');
c += strOnly([1, 2, 3].map(String).join('')) + [strOnly].length;
console.log(c);
