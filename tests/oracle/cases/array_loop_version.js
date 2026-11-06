// Loops over plain Arrays get a copy whose element ops skip the receiver's
// kind test (typed_loop_version.h, "PLAIN ARRAYS"). Each case is a loop the
// copy is entered for, with something in it the copy must still answer as
// the language does: holes, an inherited index, growth, a props object,
// frozen and sealed arrays, a receiver that is not an Array, and counters
// that leave the integer range an index has.

function copyArray(a, b) {
    for (let i = 0, l = b.length; i < l; i++) {
        a[i] = b[i];
    }
}

function arraysEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0, l = a.length; i < l; i++) {
        if (a[i] !== b[i]) return false;
    }
    return true;
}

function show(label, v) {
    console.log(label, JSON.stringify(v));
}

// Plain arrays, hot.
const src = [1, 2.5, "x", null, undefined, { k: 1 }, [2], true, -0, NaN];
for (let r = 0; r < 200; r++) {
    const dst = [];
    copyArray(dst, src);
    if (r === 199) show("copy", dst.map(String));
}
console.log("eq", arraysEqual([1, 2, 3], [1, 2, 3]), arraysEqual([1, 2, 3], [1, 2, 4]), arraysEqual([], []));

// A typed array on either side, and an array-like object.
const f32 = new Float32Array([1.5, 2.5, 3.5]);
const dst1 = [0, 0, 0];
copyArray(dst1, f32);
show("from f32", dst1);
const f64 = new Float64Array(3);
copyArray(f64, [7, 8, 9]);
show("into f64", Array.from(f64));
const like = { length: 2, 0: "a", 1: "b" };
const dst2 = [];
copyArray(dst2, like);
show("from array-like", dst2);
const obj = {};
copyArray(obj, [4, 5]);
show("into object", obj);

// Holes read through the prototype chain (Object.prototype: bronze refuses
// index properties on Array.prototype), and a setter there.
const holey = [1, , 3];
const dst3 = [];
copyArray(dst3, holey);
show("holes", dst3.length + ":" + (1 in dst3) + ":" + dst3[1]);
Object.prototype[1] = "inherited";
const dst4 = [];
copyArray(dst4, holey);
show("inherited", dst4);
let setterHits = 0;
Object.defineProperty(Object.prototype, 5, {
    configurable: true,
    get() { return "g5"; },
    set(v) { setterHits++; },
});
const dst5 = [0, 0, 0, 0, 0];
copyArray(dst5, [10, 11, 12, 13, 14, 15, 16]);
show("setter", [dst5, setterHits]);
const dst6 = [];
copyArray(dst6, new Array(7));
show("getter", dst6);
delete Object.prototype[1];
delete Object.prototype[5];

// Growth past capacity, a props object, frozen and sealed receivers.
const big = [];
for (let i = 0; i < 100; i++) big.push(i * 2);
const grow = [];
copyArray(grow, big);
console.log("grow", grow.length, grow[99]);
const withProps = [0, 0, 0];
withProps.extra = "p";
copyArray(withProps, [1, 2, 3]);
show("props", [withProps, withProps.extra]);
const frozen = Object.freeze([0, 0]);
copyArray(frozen, [1, 2]);
show("frozen", frozen);
const sealed = Object.seal([0]);
copyArray(sealed, [1, 2]);
show("sealed", sealed);
function strictCopy(a, b) {
    "use strict";
    for (let i = 0; i < b.length; i++) a[i] = b[i];
}
try {
    strictCopy(Object.freeze([0]), [1]);
    console.log("strict frozen: no throw");
} catch (e) {
    console.log("strict frozen:", e instanceof TypeError);
}

// The source shrinks and grows while the loop runs (its length was read once).
const shrinking = [1, 2, 3, 4];
const dst7 = [];
for (let i = 0, l = shrinking.length; i < l; i++) {
    dst7[i] = shrinking[i];
    if (i === 1) shrinking.length = 2;
}
show("shrink", dst7);
const growing = [1, 2];
for (let i = 0; i < 6; i++) {
    growing[i + 2] = growing[i] * 10;
}
show("grow in loop", growing);

// Counters that are not plain indices: negative, fractional, past 2^32, -0.
function readFrom(a, start, step, n) {
    const out = [];
    for (let i = start, k = 0; k < n; i += step, k++) out.push(a[i]);
    return out;
}
const arr = [10, 20, 30, 40];
arr[-1] = "minus";
arr["1.5"] = "frac";
arr[4294967296] = "big";
show("neg", readFrom(arr, -1, 1, 3));
show("frac", readFrom(arr, 0.5, 1, 3));
show("big", readFrom(arr, 4294967295, 1, 2));
function readNegZero(a) {
    const out = [];
    for (let i = -0; i < 2; i++) out.push(a[i]);
    return out;
}
show("negzero", readNegZero(arr));

// The receiver changes from an Array to a non-Array between calls.
function sum(a) {
    let s = 0;
    for (let i = 0; i < 3; i++) s += a[i];
    return s;
}
console.log("sum", sum([1, 2, 3]), sum({ 0: 1, 1: 2, 2: 3 }), sum("123"), sum(new Int8Array([1, 2, 3])));
