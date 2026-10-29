// A receiver typed as a closed object-literal shape reads its keys as Numbers
// when every literal and every write says so, and at the literal's slot behind
// a shape cell. Each section warms such a reader and then hands it an object or
// a write that the claim must not survive: the answers have to be the ones an
// ordinary property read gives.

function bump(acc, v) {
    acc.visited++;
    acc.sumX += v;
    acc.sumY += v * 2;
    return acc.visited + acc.sumX + acc.sumY;
}

// 1. Warm on literals, then one at the same keys but a different order.
let t = 0;
for (let i = 0; i < 300; i++) t = bump({ visited: 0, sumX: 0, sumY: 0 }, i);
const swapped = { sumY: 100, visited: 10, sumX: 1000 };
console.log('1', t, bump(swapped, 1), swapped.visited, swapped.sumX, swapped.sumY);

// 2. A double held where an integer was, a key added after creation, and a
// frozen literal (the store is refused in sloppy code and the value stays).
const acc = { visited: 0, sumX: 0, sumY: 0 };
for (let i = 0; i < 100; i++) bump(acc, 0.25);
acc.extra = 'e';
bump(acc, 0.5);
const frozen = Object.freeze({ visited: 1, sumX: 2, sumY: 3 });
const fr = bump(frozen, 10);
console.log('2', acc.visited, acc.sumX, acc.sumY, acc.extra, fr, frozen.visited, frozen.sumX);

// 3. Keys a reader does not own: removed with `delete`, replaced by an
// accessor, and supplied by the prototype a `__proto__` literal names.
function readPair(o) { return String(o.a) + ':' + String(o.b); }
let pr = '';
for (let i = 0; i < 100; i++) pr = readPair({ a: i, b: i + 1 });
const del = { a: 5, b: 6 };
delete del.a;
const acc2 = { a: 7, b: 8 };
Object.defineProperty(acc2, 'b', { get() { return 'got'; } });
const viaProto = { __proto__: { a: 'inherited' }, b: 9 };
console.log('3', pr, readPair(del), readPair(acc2), readPair(viaProto), Object.keys(viaProto).join(','));

// 4. Literals whose keys or values the claim must not read: spread, computed,
// getter, duplicate keys, a string from one site of the same shape.
function readXY(o) { return o.x * 10 + o.y; }
let xy = 0;
for (let i = 0; i < 100; i++) xy += readXY({ x: 1, y: 2 });
const src = { y: 'Y' };
const spread = { x: 3, ...src };
const k = 'y';
const computed = { x: 4, [k]: 'C' };
const getter = { x: 5, get y() { return 'G'; } };
const dup = { x: 6, y: 7, x: 'D' };
const strSite = { x: 'S', y: 8 };
console.log('4', xy, readXY(spread), readXY(computed), readXY(getter), readXY(dup), readXY(strSite));

// 5. Objects that never came from a literal, at the same keys: JSON.parse
// (whose objects may share the literal's shape), Object.create, a class, and
// writes through Object.assign and a computed key on a literal.
function sum3(o) { return o.p + o.q + o.r; }
let s3 = 0;
for (let i = 0; i < 100; i++) s3 += sum3({ p: 1, q: 2, r: 3 });
const parsed = JSON.parse('{"p":"a","q":"b","r":"c"}');
const parsedNum = JSON.parse('{"p":1,"q":2,"r":3.5}');
const created = Object.create({ p: 'x' });
created.q = 'y';
created.r = 'z';
class PQR { constructor() { this.p = 'c'; this.q = 'l'; this.r = 's'; } }
const assigned = { p: 1, q: 2, r: 3 };
Object.assign(assigned, { q: 'A' });
const keyed = { p: 1, q: 2, r: 3 };
for (const key of ['r']) keyed[key] = 'K';
console.log('5', s3, sum3(parsed), sum3(parsedNum), sum3(created), sum3(new PQR()), sum3(assigned), sum3(keyed));

// 6. Methods on a literal read `this`, and a reader called through call/apply
// and as a callback with other receivers.
const counter = {
    n: 0,
    step: 2,
    inc() { this.n += this.step; return this.n; },
};
for (let i = 0; i < 100; i++) counter.inc();
const other = { n: 'n', step: 's' };
const viaCall = counter.inc.call(other);
const viaApply = sum3.apply(null, [{ p: 'p', q: 1, r: 2 }]);
const mapped = [{ p: 1, q: 1, r: 1 }, { p: 'm', q: 1, r: 1 }].map(sum3).join(',');
console.log('6', counter.n, viaCall, viaApply, mapped);

// 7. A strict writer warmed on literals, then a non-writable key, a sealed
// literal adding nothing, and a setter on Object.prototype for a key the
// literal does not own.
function setV(o, v) { 'use strict'; o.v = v; o.w = v + 1; return o.v + o.w; }
let sv = 0;
for (let i = 0; i < 100; i++) sv += setV({ v: 0, w: 0 }, i);
const ro = { v: 0, w: 0 };
Object.defineProperty(ro, 'v', { writable: false });
let roErr = 'none';
try { setV(ro, 5); } catch (e) { roErr = e.constructor.name; }
const sealed = Object.seal({ v: 0, w: 0 });
const sealedR = setV(sealed, 3);
const seen = [];
Object.defineProperty(Object.prototype, 'zz', { set(v) { seen.push(v); }, configurable: true });
const lit = { v: 0, w: 0 };
lit.zz = 'proto setter';
delete Object.prototype.zz;
console.log('7', sv, roErr, ro.v, ro.w, sealedR, seen.join(','), Object.keys(lit).join(','));
