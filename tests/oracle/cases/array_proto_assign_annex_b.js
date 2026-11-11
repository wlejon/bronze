// `x.__proto__ = p` is Object.prototype's accessor (Annex B 2.2.1.2), so on
// an array, a plain object and a function it SETS the prototype and creates
// no own property, through a dotted, a string-keyed and a computed write; an
// inherited setter for any other named key runs for an array too. Then the
// Annex B accessor methods (B.2.2.2-5): __defineGetter__ and __defineSetter__
// define an enumerable, configurable accessor and refuse a non-callable;
// __lookupGetter__ and __lookupSetter__ walk the chain and answer undefined
// for a data property. Then numeric and BigInt accessor names, and the
// prototype-chain reads of the chunk's first half once more, top-level code.
'use strict';
function show(label, f) {
  try { console.log(label, String(f())); } catch (e) { console.log(label, 'threw', e.constructor.name); }
}
// bug 3: __proto__ on arrays, objects, functions
const p = { hello() { return 'hi'; } };
const a = [1, 2];
a.__proto__ = p;
show('arr proto set', () => Object.getPrototypeOf(a) === p);
show('arr own __proto__', () => Object.prototype.hasOwnProperty.call(a, '__proto__'));
show('arr method', () => a.hello());
const b = [3];
b['__proto__'] = null;
show('arr proto null', () => Object.getPrototypeOf(b) === null);
const k = '__proto__';
const c = [4];
c[k] = Array.prototype;
show('arr same proto', () => Object.getPrototypeOf(c) === Array.prototype && Object.keys(c).length);
const o = {};
o.__proto__ = p;
show('obj proto', () => Object.getPrototypeOf(o) === p);
function fn() {}
fn.__proto__ = p;
show('fn proto', () => Object.getPrototypeOf(fn) === p);
show('arr __proto__ get', () => [].__proto__ === Array.prototype);
// inherited setter for a named key on an array
let seen = [];
const d = [];
d.zz = 7;
show('inherited named setter', () => seen.join(',') + ' own=' + Object.prototype.hasOwnProperty.call(d, 'zz'));
delete Object.prototype.zz;
// bug 4: Annex B
const e = {};
e.__defineGetter__('x', function () { return 42; });
e.__defineSetter__('y', function (v) { this._y = v * 2; });
e.y = 5;
show('defineGetter', () => e.x + ' ' + e._y);
show('lookupGetter', () => e.__lookupGetter__('x')());
show('lookupSetter', () => typeof e.__lookupSetter__('y'));
show('lookup data', () => e.__lookupGetter__('_y'));
show('lookup missing', () => e.__lookupGetter__('nope'));
show('lookup inherited', () => typeof Object.create(e).__lookupGetter__('x'));
show('desc', () => JSON.stringify(Object.getOwnPropertyDescriptor(e, 'x'), (k, v) => typeof v === 'function' ? 'fn' : v));
show('define non-callable', () => e.__defineGetter__('q', 1));
show('define on frozen', () => Object.freeze({}).__defineGetter__('q', () => 1));
show('lengths', () => [Object.prototype.__defineGetter__.length, Object.prototype.__lookupSetter__.length].join());
show('lookup proto __proto__', () => typeof Object.prototype.__lookupGetter__('__proto__'));
// bug 5: numeric accessor names
const lit = { get 4() { return 'four'; }, set 2(v) { this.two = v; }, get 0x10() { return 'sixteen'; }, get 1.5() { return 'f'; }, get 1n() { return 'big'; }, 7: 'seven', 8n: 'eight' };
lit[2] = 'x';
show('lit', () => [lit[4], lit.two, lit[16], lit['1.5'], lit[1], lit[7], lit[8]].join());
class C { get 4() { return 'c4'; } set 5(v) { this.v = v; } static get 6() { return 's6'; } get 2n() { return 'c2'; } }
const ci = new C();
ci[5] = 9;
show('class', () => [ci[4], ci.v, C[6], ci[2]].join());
show('names', () => Object.getOwnPropertyDescriptor(C.prototype, '4').get.name + '|' + Object.getOwnPropertyDescriptor(lit, '16').get.name + '|' + Object.getOwnPropertyDescriptor(lit, '1').get.name);

t2('objproto read', () => { Object.prototype[3] = 'x'; const r = [1][3]; delete Object.prototype[3]; return r; });
t2('objproto hole', () => { Object.prototype[1] = 'y'; const r = [1, , 3][1]; delete Object.prototype[1]; return r; });
t2('arrproto in', () => { const p = Object.create(Array.prototype); p[2] = 'z'; const d = [0, 1]; Object.setPrototypeOf(d, p); return [2 in d, d[2], d.length]; });
t2('__proto__ arr', () => { const p = Object.create(Array.prototype); p[2] = 'z'; const d = [0, 1]; d.__proto__ = p; return [Object.getPrototypeOf(d) === p, d[2], Object.keys(d).join()]; });
t2('__proto__ obj', () => { const p = { a: 1 }; const o = {}; o.__proto__ = p; return [Object.getPrototypeOf(o) === p, o.a, Object.keys(o).join()]; });
t2('__proto__ computed', () => { const p = { a: 1 }; const o = {}; o['__proto__'] = p; return [Object.getPrototypeOf(o) === p, o.a]; });
t2('__proto__ get', () => { const d = [1]; return d.__proto__ === Array.prototype; });
t2('defineGetter', () => { const o = {}; o.__defineGetter__('x', () => 5); return [o.x, typeof o.__lookupGetter__('x')]; });
t2('ta proto', () => { const ta = new Uint8Array([7, 8, 9]); const a = [1]; Object.setPrototypeOf(a, ta); return [a[2], 2 in a]; });
t2('arr as proto', () => { const q = [5, 6, 7]; const a = [1]; Object.setPrototypeOf(a, q); return [a[2], 2 in a]; });
t2('args proto', () => { const ar = (function () { return arguments; })(4, 5, 6); const a = [1]; Object.setPrototypeOf(a, ar); return [a[2], 2 in a]; });
t2('proxy proto', () => { const log = []; const px = new Proxy({}, { get(t, k) { log.push('g' + String(k)); return 'P'; }, has(t, k) { log.push('h' + String(k)); return true; } }); const a = [1]; Object.setPrototypeOf(a, px); return [a[3], 3 in a, log.join()]; });
function t2(name, f) { try { console.log(name, f()); } catch (e) { console.log(name, 'threw', e.constructor.name, e.message); } }
