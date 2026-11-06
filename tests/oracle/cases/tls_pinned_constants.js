// The NaN-box tags, payload mask and limit, and the other constants guards
// compare with are read from below the thread's block (abi/bronze_abi_tls.h,
// BRONZE_TLS_CONSTANTS) rather than from each function's constant pool. Every
// kind of value through the guards and conversions that test those words:
// object/number/string/boolean/null/undefined tests, holes, NaN, 1.0, and
// comparisons in loops hot enough to be compiled.

function kind(v) {
    if (v === undefined) return 'undefined';
    if (v === null) return 'null';
    if (v === true) return 'true';
    if (v === false) return 'false';
    if (typeof v === 'number') return Number.isNaN(v) ? 'NaN' : (v === 1 ? 'one' : 'number');
    if (typeof v === 'string') return 'string';
    if (typeof v === 'object') return 'object';
    return typeof v;
}

const values = [undefined, null, true, false, 0, -0, 1, 1.0, 1.5, -1, NaN, Infinity, -Infinity,
    2 ** 31, 2 ** 32, 2 ** 48, 2 ** 53, -(2 ** 53), 'a', '', {}, [], [1, , 3], 0x7fffffff, -0x80000000];

let line = [];
for (const v of values) line.push(kind(v));
console.log(line.join(' '));

// Truthiness and loose/strict equality against each tag.
function truthy(v) { return v ? 1 : 0; }
function looseNull(v) { return v == null ? 1 : 0; }
function strictOne(v) { return v === 1 ? 1 : 0; }
let acc = [0, 0, 0];
for (let r = 0; r < 2000; r++) {
    for (const v of values) {
        acc[0] += truthy(v);
        acc[1] += looseNull(v);
        acc[2] += strictOne(v);
    }
}
console.log(acc.join(' '));

// Holes read as undefined; the hole tag is never seen.
const holes = [1, , 3, , 5];
let seen = [];
for (let i = 0; i < holes.length; i++) seen.push(holes[i] === undefined ? 'u' : String(holes[i]));
console.log(seen.join(','), 1 in holes, 2 in holes);

// Objects through property guards, numbers through arithmetic with 1.0.
class P { constructor(x, y) { this.x = x; this.y = y; } }
function step(ps) {
    let s = 0;
    for (const p of ps) {
        p.x = p.x * 1.0 + 1.0;
        p.y = p.y === undefined ? null : p.y;
        s += p.x + (p.y === null ? 0 : p.y);
    }
    return s;
}
const ps = [];
for (let i = 0; i < 50; i++) ps.push(new P(i, i % 3 === 0 ? undefined : i / 2));
let total = 0;
for (let r = 0; r < 200; r++) total += step(ps);
console.log(total, ps[3].y, ps[4].y, ps[49].x);

// Booleans and NaN stored in arrays and compared.
const flags = [];
for (let i = 0; i < 64; i++) flags.push(i % 5 === 0, i % 7 === 0 ? NaN : i);
let t = 0, f = 0, n = 0, num = 0;
for (const v of flags) {
    if (v === true) t++;
    else if (v === false) f++;
    else if (v !== v) n++;
    else num += v;
}
console.log(t, f, n, num);
