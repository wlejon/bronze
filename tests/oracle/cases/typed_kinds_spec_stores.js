// Every typed-array element kind read and written in a hot loop (so a build
// on its own harvest takes each site's inline form for that one kind), with
// values each kind converts: fractions, negatives, values past the kind's
// range, past 2^32 and 2^53, -0, NaN, the infinities, and Uint8Clamped's
// ties (rounded to even). Then each view is read back as bytes.
const values = [0, -0, 1, -1, 0.5, 1.5, 2.5, -1.5, 127.5, 128, 254.5, 255.5, 256, 300.7, -129, 32767.9, 32768,
    65535, 65536.25, -32769, 2147483647, 2147483648, -2147483649, 4294967295, 4294967296, 4294967297.5,
    9007199254740993, 1e20, -1e20, 1.5e300, NaN, Infinity, -Infinity, 3.4e38, 3.5e38, 1e-40, 0.1];
const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
    Float32Array, Float64Array];

function fill(view, src, n) {
    for (let i = 0; i < n; i++) view[i] = src[i];
}
function sum(view, n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += view[i];
    return s;
}
function bytes(view) {
    const u = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    let h = 0;
    for (let i = 0; i < u.length; i++) h = (Math.imul(h, 31) + u[i]) | 0;
    return (h >>> 0).toString(16);
}
// One pair of functions per kind, so each site meets one kind.
function fillInt8Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumInt8Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillUint8Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumUint8Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillUint8ClampedArray(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumUint8ClampedArray(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillInt16Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumInt16Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillUint16Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumUint16Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillInt32Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumInt32Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillUint32Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumUint32Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillFloat32Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumFloat32Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
function fillFloat64Array(view, src, n) { for (let i = 0; i < n; i++) view[i] = src[i]; }
function sumFloat64Array(view, n) { let s = 0; for (let i = 0; i < n; i++) s += view[i]; return s; }
const perKind = [[fillInt8Array, sumInt8Array], [fillUint8Array, sumUint8Array], [fillUint8ClampedArray, sumUint8ClampedArray], [fillInt16Array, sumInt16Array], [fillUint16Array, sumUint16Array], [fillInt32Array, sumInt32Array], [fillUint32Array, sumUint32Array], [fillFloat32Array, sumFloat32Array], [fillFloat64Array, sumFloat64Array]];
const n = values.length;
for (let k = 0; k < kinds.length; k++) {
    const K = kinds[k];
    const [fillK, sumK] = perKind[k];
    const view = new K(n + 3).subarray(1, n + 1);
    let s = 0;
    for (let r = 0; r < 3000; r++) {
        fillK(view, values, n);
        s += sumK(view, n);
    }
    const out = [];
    for (let i = 0; i < n; i++) out.push(Object.is(view[i], -0) ? '-0' : String(view[i]));
    console.log(K.name, s, bytes(view), out.join(' '));
}
// The shared functions see every kind: the generic path's conversions.
for (const K of kinds) {
    const view = new K(n);
    fill(view, values, n);
    console.log(K.name, sum(view, n), bytes(view));
}
