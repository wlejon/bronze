// o[k] = v with k a run-time key and o of a proven few-field class is
// compiled as "k is the interned name of field f? store field f : generic
// store" (emitKeyedFieldStore). Each case is a key or a receiver the fast
// arms must not take, or a store they must do exactly as the generic one.

class V3 {
    constructor(x, y, z) { this.x = x; this.y = y; this.z = z; }
}

function setKey(v, k, val) { v[k] = val; return v; }
function setKeyStrict(v, k, val) { 'use strict'; v[k] = val; return v; }
function dump(v) {
    const own = Object.keys(v).map(k => k + '=' + String(v[k]));
    return own.join(' ');
}

const axes = ['x', 'y', 'z'];
const v = new V3(1, 2, 3);
for (let i = 0; i < 30; i++) setKey(v, axes[i % 3], i);
console.log('axes', dump(v));

// Strings equal to a field name but not the interned constant.
const built = ['x' + '', 'xy'.slice(0, 1), String.fromCharCode(121), ['z'].join('')];
for (const k of built) setKey(v, k, k + '!');
console.log('built', dump(v));

// Other strings add properties; numbers and symbols are other keys.
setKey(v, 'w', 4); setKey(v, 'X', 5); setKey(v, '', 6); setKey(v, 'xx', 7);
setKey(v, 0, 'zero'); setKey(v, 1.5, 'frac');
const sym = Symbol('x');
setKey(v, sym, 'sym');
console.log('others', dump(v), String(v[sym]));

// An object key converts with ToPropertyKey once, after the receiver.
const log = [];
const keyObj = { toString() { log.push('toString'); return 'y'; } };
const rhs = () => { log.push('rhs'); return 'fromObj'; };
const recv = () => { log.push('recv'); return v; };
recv()[(log.push('key'), keyObj)] = rhs();
console.log('order', log.join(','), v.y);

// A receiver of another shape takes the generic store.
const plain = { y: 0, x: 0 };
setKey(plain, 'x', 9); setKey(plain, 'z', 8);
console.log('plain', dump(plain));
const sub = new (class extends V3 { constructor() { super(0, 0, 0); this.q = 1; } })();
setKey(sub, 'q', 2); setKey(sub, 'z', 3);
console.log('sub', dump(sub));

// Frozen: sloppy ignores, strict throws.
const fz = Object.freeze(new V3(1, 1, 1));
setKey(fz, 'x', 5);
try { setKeyStrict(fz, 'y', 5); console.log('no throw'); }
catch (e) { console.log('frozen', e instanceof TypeError); }
console.log('frozen', dump(fz));

// A non-writable own field.
const nw = new V3(1, 2, 3);
Object.defineProperty(nw, 'z', { writable: false });
setKey(nw, 'z', 30);
try { setKeyStrict(nw, 'z', 31); console.log('no throw'); }
catch (e) { console.log('nonwritable', e instanceof TypeError); }
setKey(nw, 'x', 10);
console.log('nonwritable', dump(nw));

// A field turned into an accessor on the instance.
const acc = new V3(0, 0, 0);
let seen = [];
Object.defineProperty(acc, 'y', { get() { return 'g'; }, set(val) { seen.push(val); }, configurable: true });
setKey(acc, 'y', 'via-setter'); setKey(acc, 'x', 'direct');
console.log('accessor', seen.join(','), acc.y, acc.x);

// A setter on the prototype for a name that is not an own field yet.
class P2 { constructor() { this.a = 1; this.b = 2; } }
const p2 = new P2();
Object.defineProperty(P2.prototype, 'c', { set(val) { seen.push('c:' + val); } });
setKey(p2, 'c', 3); setKey(p2, 'a', 'A');
console.log('protosetter', seen.join(','), dump(p2));

// A class with more fields than the fast arms take.
class Big { constructor() { this.a = 1; this.b = 2; this.c = 3; this.d = 4; this.e = 5; this.f = 6; } }
const bg = new Big();
for (const k of ['a', 'b', 'c', 'd', 'e', 'f', 'g']) setKey(bg, k, k.toUpperCase());
console.log('big', dump(bg));

// Compound assignment and update through a key.
const cv = new V3(1, 2, 3);
for (const k of axes) { cv[k] += 10; cv[k] *= 2; cv[k]++; }
console.log('compound', dump(cv));

// The assignment's value is the stored value.
const w = new V3(0, 0, 0);
let k2 = 'z';
const r = (w[k2] = 'val');
console.log('value', r, w.z);

// Prototype-chain keys like __proto__ and constructor.
const pv = new V3(1, 2, 3);
setKey(pv, 'constructor', 'c');
setKey(pv, 'toString', 't');
console.log('protonames', dump(pv), Object.getPrototypeOf(pv) === V3.prototype);
