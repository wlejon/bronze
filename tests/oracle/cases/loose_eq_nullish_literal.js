// `x == null` / `x != null` and the `undefined` spellings, either way round:
// true for null and undefined only, whatever else the value coerces to.
const values = [
    null, undefined, 0, -0, NaN, '', '0', 'null', 'undefined', false, true,
    [], [null], [undefined], {}, { valueOf() { return null; } },
    { toString() { return 'null'; } }, () => null, Symbol('s'), 1.5, Infinity,
];
function label(v) {
    if (typeof v === 'symbol') return 'symbol';
    if (typeof v === 'function') return 'function';
    if (typeof v === 'object' && v !== null) return Array.isArray(v) ? 'array' + v.length : 'object';
    if (typeof v === 'string') return JSON.stringify(v);
    return Object.is(v, -0) ? '-0' : String(v);
}
for (const v of values) {
    console.log(label(v), v == null, v != null, null == v, null != v,
        v == undefined, v != undefined, undefined == v, undefined != v);
}

// A sparse array's hole reads as undefined.
const sparse = [1, , 3];
console.log(sparse[1] == null, sparse[1] != null, sparse[5] == null);

// Both operands are evaluated, left to right, and nothing is coerced.
const order = [];
const probe = {
    valueOf() { order.push('valueOf'); return 0; },
    toString() { order.push('toString'); return ''; },
    [Symbol.toPrimitive]() { order.push('toPrimitive'); return null; },
};
function get(name, v) { order.push(name); return v; }
console.log(get('a', probe) == null, null != get('b', probe), order.join(','));

// The shapes a bundler leaves for `a ?? b` and `a?.b`.
function pick(x, fallback) { var _a; return (_a = x) != null ? _a : fallback; }
function reach(o) { return o == null ? void 0 : o.name; }
console.log(pick(0, 'd'), pick('', 'd'), pick(false, 'd'), pick(null, 'd'), pick(undefined, 'd'), pick(NaN, 'd'));
console.log(reach(null), reach(undefined), reach({ name: 'n' }), reach('str'), reach(0));

// Typed operands: numbers, strings and booleans the compiler has proven.
function typed(n, s, b) {
    const num = n * 2, str = s + '!', flag = !b;
    return [num == null, num != null, str == null, null != str, flag == undefined, undefined != flag].join(' ');
}
console.log(typed(1, 'a', true), typed(NaN, '', false));

// In conditions, loops and with a literal on both sides.
let count = 0;
for (let node = { next: { next: { next: null } } }; node != null; node = node.next) count++;
console.log(count, null == null, null == undefined, undefined != null, null != null);
let seen = 0;
const list = [1, null, 2, undefined, 3];
for (let i = 0; i < list.length; i++) { if (list[i] == null) continue; seen += list[i]; }
console.log(seen);

// Strict comparison still tells the two apart.
console.log(null === undefined, undefined === undefined, null !== undefined, void 0 == null);
