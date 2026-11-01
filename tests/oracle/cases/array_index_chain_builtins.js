// An index the array does not own (a hole, or one past the length) is looked
// up on the prototype chain by every [[Get]], [[HasProperty]] and [[Set]]
// (10.1.8.1, 10.1.7.1, 10.1.9.2): a data element on Object.prototype reads
// through, an inherited accessor's getter runs, and a Set into a hole or past
// the end runs an inherited setter, stops at an inherited non-writable
// element, and goes through a Proxy's `set` trap, whether it comes from
// `a[i] = v`, push, unshift, splice, fill, copyWithin, reverse or Reflect.set.
// The array's own fast paths only take a dense element; everything here is
// the walk they hand over to once some prototype holds an index key.
'use strict';
let out = '';
function show(label, f) {
  try { out += label + ' ' + JSON.stringify(f()) + '\n'; } catch (e) { out += label + ' threw ' + e.constructor.name + '\n'; }
}
Object.prototype[1] = 'P1';
Object.defineProperty(Object.prototype, 3, {
  get() { return 'G3'; },
  set(v) { Object.defineProperty(this, 3, { value: v, writable: true, enumerable: true, configurable: true }); },
  configurable: true,
});
const h = () => [0, , 2, , 4];
show('index', () => { const a = h(); return [a[1], a[3], a[7]]; });
show('iter', () => [...h()]);
show('forof', () => { const r = []; for (const x of h()) r.push(x); return r; });
show('destructure', () => { const [p, q, r, s] = h(); return [p, q, r, s]; });
show('apply', () => Array.prototype.concat.apply([], [h()]).length + ':' + Math.max.apply(null, [0, , 2].map(String).map(Number)));
show('fnapply', () => (function () { return [...arguments]; }).apply(null, h()));
show('join', () => h().join('-'));
show('toString', () => String(h()));
show('json', () => JSON.stringify(h()));
show('forin', () => { const r = []; for (const k in h()) r.push(k); return r; });
show('keys', () => Object.keys(h()));
show('indexOf', () => [h().indexOf('P1'), h().includes('G3'), h().lastIndexOf('P1')]);
show('slice', () => h().slice(0, 4));
show('concat', () => [].concat(h()));
show('map', () => h().map((x) => x));
show('forEach', () => { const r = []; h().forEach((x, i) => r.push(i + '=' + x)); return r; });
show('filter', () => h().filter(() => true));
show('reduce', () => h().reduce((acc, x) => acc + '|' + x, ''));
show('some', () => { const r = []; h().some((x) => { r.push(x); return false; }); return r; });
show('find', () => [h().find((x) => x === 'P1'), h().findIndex((x) => x === 'G3'), h().findLast((x) => x === 'P1')]);
show('at', () => [h().at(1), h().at(-2)]);
show('from', () => Array.from(h()));
show('flat', () => [h()].flat());
show('reverse', () => h().reverse());
show('copyWithin', () => h().copyWithin(0, 1));
show('sort', () => h().sort());
show('shift', () => { const a = h(); const x = [a.shift(), a.shift()]; return [x, a]; });
show('splice', () => { const a = h(); const r = a.splice(1, 2); return [r, a]; });
show('unshift', () => { const a = h(); a.unshift(9); return a; });
show('fill', () => { const a = h(); a.fill(7, 0, 2); return a; });
show('assign', () => Object.assign([], h()));
show('spreadobj', () => ({ ...h() }));
show('entries', () => [...h().entries()]);
show('values', () => [...h().values()]);
show('with', () => h().with(0, 'w'));
show('toSorted', () => h().toSorted());
show('toReversed', () => h().toReversed());
show('toSpliced', () => h().toSpliced(0, 1));
show('flatMap', () => h().flatMap((x) => [x]));
show('every', () => h().every((x) => x !== undefined));
show('in', () => [1 in h(), 3 in h(), 5 in h()]);
show('hasOwn', () => [Object.hasOwn(h(), 1), h().hasOwnProperty(3)]);
show('typed from', () => Array.from(new Float64Array(h())));
show('set', () => [...new Set(h())]);
show('str fromCharCode', () => String.fromCharCode.apply(null, [65, , 67].map((x) => x === undefined ? 66 : x)));
show('obj literal hole', () => { const o = { length: 5, 0: 'a' }; return Array.prototype.slice.call(o); });
delete Object.prototype[1];
delete Object.prototype[3];
console.log(out);
out = '';
// a custom chain with a setter on an index
const log = [];
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, 2, { set(v) { log.push('set2=' + v); }, get() { return 'pg2'; }, configurable: true });
const c = [];
Object.setPrototypeOf(c, proto);
c.push('a', 'b', 'c');
show('custom push', () => [log, c.length, c[2], Object.hasOwn(c, 2)]);
c.length = 0; log.length = 0;
c.unshift('x', 'y', 'z');
show('custom unshift', () => [log, c.length]);
c.length = 0; log.length = 0;
c.splice(0, 0, 'p', 'q', 'r');
show('custom splice', () => [log, c.length]);
c.length = 0; log.length = 0;
c.length = 4; c.fill('f');
show('custom fill', () => [log, Object.hasOwn(c, 2)]);
c.length = 0; log.length = 0;
c[0] = 1; c[1] = 2; c[2] = 3; c[3] = 4;
show('custom index', () => [log, c.length]);
show('custom concat', () => { const r = [1, , 3]; Object.setPrototypeOf(r, proto); return Array.prototype.concat.call([], r); });
show('custom copyWithin', () => { log.length = 0; const r = [1, 2, 9, 4]; delete r[2]; Object.setPrototypeOf(r, proto); r.copyWithin(2, 0); return log; });
show('custom reverse', () => { log.length = 0; const r = [1, 2, , 4, 5]; Object.setPrototypeOf(r, proto); r.reverse(); return log; });
show('reflect set', () => { log.length = 0; const r = []; Object.setPrototypeOf(r, proto); return [Reflect.set(r, 2, 'R'), log]; });
// non-writable inherited index
const frozenProto = Object.create(Array.prototype);
Object.defineProperty(frozenProto, 0, { value: 'ro', writable: false, configurable: true });
const f = [];
Object.setPrototypeOf(f, frozenProto);
show('nonwritable index', () => { f[0] = 'w'; return f[0]; });
show('nonwritable push', () => { f.push('w'); return f.length; });
// proxy in the chain
const plog = [];
const px = new Proxy(Object.create(Array.prototype), { set(t, k, v, r) { plog.push('set ' + String(k)); return Reflect.set(t, k, v, r); }, get(t, k, r) { if (k === '1') return 'PX1'; return Reflect.get(t, k, r); }, has(t, k) { return k === '1' || Reflect.has(t, k); } });
const pa = [0, , 2];
Object.setPrototypeOf(pa, px);
pa[5] = 'five';
show('proxy chain', () => [plog, pa[1], 1 in pa, pa.length, Object.hasOwn(pa, 5)]);
console.log(out);
