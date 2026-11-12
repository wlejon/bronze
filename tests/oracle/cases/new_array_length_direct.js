// `new Array()` and `new Array(n)`: n holes (not n undefineds), a length that
// is no array length a RangeError, any other single argument an element, and
// the instance NewTarget asks for.
const out = [];
const desc = (a) => [Array.isArray(a), a.length, Object.keys(a).length, 0 in a, JSON.stringify(a)].join(' ');
out.push(desc(new Array()), desc(new Array(0)), desc(new Array(-0)), desc(new Array(1)), desc(new Array(3)));
for (const n of [4095, 4096, 4097, 70000]) {
    const a = new Array(n);
    let visited = 0;
    a.forEach(() => visited++);
    out.push([a.length, visited, n - 1 in a, a[n - 1], a.indexOf(undefined), a.includes(undefined)].join(' '));
}
for (const bad of [-1, 1.5, NaN, Infinity, -Infinity, 4294967296, 2 ** 53]) {
    try { new Array(bad); out.push('no throw ' + bad); } catch (e) { out.push(e instanceof RangeError ? 'RangeError' : 'other'); }
}
out.push(desc(new Array('3')), desc(new Array(true)), desc(new Array(null)), desc(new Array(undefined)), desc(new Array([2])));
out.push(new Array(3n).length, typeof new Array(3n)[0], desc(new Array({ valueOf() { return 2; } })), desc(new Array(new Number(2))));
out.push(desc(new Array(3, 4)), desc(new Array(1, 2, 3)), desc(Array(2)), desc(Array()), desc(Array('x', 'y')));
// The holes fill like any array's, and the array grows past its length.
const a = new Array(2);
a[1] = 'b'; a.push('c'); a[5] = 'f';
out.push(desc(a), a.join('-'));
const lw = new Array(3);
for (let i = 0; i < lw.length; i++) lw[i] = i * 1.5;
out.push(desc(lw), lw.reduce((x, y) => x + y, 0));
// Subclasses, a bound constructor and Reflect.construct keep the protocol.
class Sub extends Array { first() { return this[0]; } }
const s = new Sub(3);
out.push([s instanceof Sub, s instanceof Array, s.length, 0 in s, typeof s.first].join(' '));
const s2 = new Sub(7, 8);
out.push([s2.length, s2.first(), s2.map((x) => x + 1) instanceof Sub].join(' '));
const Bound = Array.bind(null, 2);
out.push(desc(new Bound()), desc(new Bound(9)));
const r = Reflect.construct(Array, [3], Sub);
out.push([r instanceof Sub, r.length, 0 in r].join(' '));
const r2 = Reflect.construct(Array, [2]);
out.push([Object.getPrototypeOf(r2) === Array.prototype, r2.length].join(' '));
out.push(Object.getPrototypeOf(new Array(2)) === Array.prototype, new Array(2).constructor === Array);
// One site, many lengths.
function make(n) { return new Array(n); }
let total = 0;
for (let i = 0; i < 500; i++) { const m = make(i % 7); m[0] = i; total += m.length; }
out.push(total);
const Ctor = Array;
out.push(desc(new Ctor(2)), desc(new Ctor));
console.log(out.join('\n'));
