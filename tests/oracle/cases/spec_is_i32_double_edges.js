// is.i32 on a double made in the function (box.f64) and on a value of any
// type: integral doubles in int32 range index arrays as integers; -0, a
// fraction, NaN, infinities and integral values past int32 do not, and every
// other type is no int32. The answers must not depend on training.
const arr = [];
for (let i = 0; i < 16; i++) arr.push(i * 10);
const obj = { '1.5': 'one and a half', '4294967296': 'two to the 32', 'NaN': 'not a number',
              'Infinity': 'inf', '-Infinity': 'minus inf', '2147483648': 'two to the 31' };
arr['1.5'] = 'frac';

function at(a, x) {
    const d = x * 0.5;  // a double made here
    return a[d];
}
function atAny(a, k) { return a[k]; }
function isInt(x) {
    const d = x / 3;
    return Number.isInteger(d) && d >= -2147483648 && d <= 2147483647 ? (d | 0) : 'no';
}
function sw(x) {
    const d = x * 1.0 + 0;
    switch (d) {
        case 0: return 'zero';
        case 1: return 'one';
        case -1: return 'minus one';
        case 2147483647: return 'max';
        case -2147483648: return 'min';
        default: return 'other ' + d;
    }
}

let sum = 0;
for (let r = 0; r < 3000; r++) sum += at(arr, (r % 16) * 2);
console.log('trained', sum);

const edges = [0, -0, 1, 3, 30, 31, 2 * 2147483647, -2 * 2147483648, 4294967296 * 2, NaN, Infinity, -Infinity,
               1e300, -1e300, 2 ** 64, -(2 ** 63) * 2, 2 ** 63 * 2, 0.5, -0.5, 5e-324];
for (const e of edges) console.log('at', e, String(at(arr, e)), String(at(obj, e)));

for (let r = 0; r < 3000; r++) sum += atAny(arr, r % 16);
const keys = [0, -0, 1.5, 15, 16, 2147483648, -1, 4294967296, NaN, '3', '03', true, null, undefined, {}, [2],
              Symbol.iterator, 10n, Infinity, -Infinity, 2 ** 31 - 1, -(2 ** 31)];
for (const k of keys) {
    let v;
    try { v = atAny(arr, k); } catch (e) { v = 'threw ' + e.constructor.name; }
    console.log('any', typeof k === 'symbol' ? 'symbol' : String(k), typeof k, String(v));
}

for (let r = 0; r < 3000; r++) sum += isInt(r * 3) === 'no' ? 1 : 0;
for (const e of [0, -0, 3, -3, 1, 6442450941, 6442450944, -6442450944, NaN, 1e20, 4.5])
    console.log('isInt', e, isInt(e));

for (let r = 0; r < 3000; r++) sw(r % 2);
for (const e of [0, -0, 1, -1, 2147483647, -2147483648, 2147483648, 0.5, NaN, Infinity, 1 + 2 ** -52])
    console.log('sw', e, sw(e));
console.log('sum', sum);
