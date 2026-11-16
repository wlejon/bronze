// A double slot that generalizes moves its objects to the boxed sibling of
// their shape. Sites speculated on the double shape read, write and add on
// those siblings through the generic access, with the same results.

function Pt(x, y) {
    this.x = x;
    this.y = y;
}
function sum(arr) {
    let s = 0;
    for (let i = 0; i < arr.length; i++) s += arr[i].y + arr[i].x;
    return s;
}
function scale(arr, k) {
    for (let i = 0; i < arr.length; i++) arr[i].y = arr[i].y * k;
}
function tag(o, v) {
    o.z = v;
    return o;
}

const arr = [];
for (let i = 0; i < 64; i++) arr.push(new Pt(i * 0.5, i));
let total = 0;
for (let r = 0; r < 300; r++) {
    if (r === 150) {
        // Generalize x on half the objects, keep the values Numbers.
        for (let i = 0; i < arr.length; i += 2) {
            const keep = arr[i].x;
            arr[i].x = 'tmp';
            arr[i].x = keep;
        }
    }
    total += sum(arr);
    scale(arr, r % 2 === 0 ? 2 : 0.5);
}
console.log(total);

// Adds onto siblings: a fresh Pt gets z as a Number, a generalized one too.
const a = tag(new Pt(1, 2), 3);
const b = new Pt(1, 2);
b.y = null;
b.y = 2;
tag(b, 4);
console.log(a.x + a.y + a.z, b.x + b.y + b.z);
for (let i = 0; i < 200; i++) tag(i % 3 === 0 ? b : new Pt(i, i), i);
console.log(b.z, Object.keys(b).join());

// A sibling whose slot now holds a non-Number reads it as stored.
const c = new Pt(5, 6);
c.x = 'five';
console.log(sum([c, new Pt(1, 1)]));
