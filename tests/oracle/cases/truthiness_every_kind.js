// ToBoolean of every kind of value, through `if`, `!`, `&&`, `||`, `?:` and a
// loop condition, in code hot enough to be compiled.
const sym = Symbol('s');
const values = [true, false, null, undefined, 0, -0, NaN, 1, -1, 0.5, Infinity, -Infinity, 1e-320,
    '', 'a', '0', 'false', 0n, 1n, -1n, sym, {}, [], [0], () => 0, new Boolean(false), new Number(0), new String(''),
    2147483647, -2147483648, 4294967296, Object.create(null), /x/, new Date(0), new Map(), 1 << 3, 5 | 0, 7 >>> 0, 0 | 0];
const names = values.map((v) => typeof v === 'symbol' ? 'sym' : typeof v === 'bigint' ? v + 'n' : Object.is(v, -0) ? '-0' :
    typeof v === 'object' && v !== null ? Object.prototype.toString.call(v) : typeof v === 'function' ? 'fn' : JSON.stringify(v) ?? String(v));
function viaIf(v) { if (v) return 1; return 0; }
function viaNot(v) { return !v ? 0 : 1; }
function viaAnd(v) { return (v && 1) === 1 ? 1 : 0; }
function viaOr(v) { return (v || 0) === 0 ? 0 : 1; }
function viaTernary(v) { return v ? 1 : 0; }
function viaWhile(v) { let n = 0; while (v) { n++; v = null; } return n; }
function viaHole(a, i) { return a[i] ? 1 : 0; }
const counts = [0, 0, 0, 0, 0, 0];
let rows = [];
for (let round = 0; round < 400; round++) {
    for (let i = 0; i < values.length; i++) {
        const v = values[i];
        const r = [viaIf(v), viaNot(v), viaAnd(v), viaOr(v), viaTernary(v), viaWhile(v)];
        for (let k = 0; k < 6; k++) counts[k] += r[k];
        if (round === 399) rows.push(names[i] + ':' + r.join(''));
    }
}
console.log(counts.join(' '));
console.log(rows.join(' '));
// Fields that hold a flag, an object or nothing, tested as conditions.
class Node { constructor(i) { this.visible = i % 3 !== 0; this.map = i % 4 === 0 ? { id: i } : i % 4 === 1 ? null : undefined; this.count = i % 5; this.name = i % 7 === 0 ? '' : 'n' + i; } }
const nodes = [];
for (let i = 0; i < 64; i++) nodes.push(new Node(i));
let visible = 0, mapped = 0, counted = 0, named = 0, both = 0, either = 0;
for (let round = 0; round < 500; round++) {
    for (let i = 0; i < nodes.length; i++) {
        const n = nodes[i];
        if (n.visible) visible++;
        if (n.map) mapped++;
        if (n.count) counted++;
        if (n.name) named++;
        if (n.visible && n.map) both++;
        if (n.map || n.count) either++;
    }
}
console.log(visible, mapped, counted, named, both, either);
// Holes and out-of-range reads.
const holey = [1, , 0, , 'x', , null];
let h = 0;
for (let round = 0; round < 300; round++) for (let i = 0; i < 9; i++) h += viaHole(holey, i);
console.log(h);
