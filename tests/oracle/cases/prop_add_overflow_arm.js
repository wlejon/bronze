// Property adds past an object's inline slots, answered by the write stub's
// add arm while the overflow block has room, and the adds it must hand to
// the runtime: the first overflow add (no block yet), a full block, a frozen
// or non-extensible object, a setter on the prototype for a key the add
// would create, and a derived class of a function base (few predicted
// inline slots) built many times.
function Base() { this.listeners = null; this.count = 0; }
Base.prototype.on = function () { this.count++; return this; };
class Style extends Base {
    constructor(i) {
        super();
        this.a = i; this.b = i + 1; this.c = 'c' + i; this.d = i * 2; this.e = null;
        this.f = i % 2 === 0; this.g = [i]; this.h = { i }; this.j = 1.5 * i; this.k = 'k';
        this.l = i; this.m = i; this.n = i; this.o = i; this.p = i; this.q = i;
    }
}
let sum = 0;
for (let i = 0; i < 200; i++) {
    const s = new Style(i);
    sum += s.a + s.b + s.d + s.j + s.q + s.g[0] + s.h.i + (s.f ? 1 : 0) + s.on().count;
}
console.log(sum);

// Incremental adds onto one object, far past inline capacity and through
// several overflow growths, read back by name and by key.
function grow(o, n, tag) {
    for (let i = 0; i < n; i++) o[tag + i] = i * 3;
    let t = 0;
    for (let i = 0; i < n; i++) t += o[tag + i];
    return t + ':' + Object.keys(o).length;
}
console.log(grow({}, 150, 'p'), grow({ x: 1 }, 70, 'q'));
function named() {
    const o = {};
    o.a0 = 0; o.a1 = 1; o.a2 = 2; o.a3 = 3; o.a4 = 4; o.a5 = 5; o.a6 = 6; o.a7 = 7;
    return o;
}
for (let r = 0; r < 3; r++) { const o = named(); console.log(Object.keys(o).join(), o.a7); }

// Adds the arm must refuse.
const frozen = Object.freeze(new Style(1));
try { 'use strict'; (function () { 'use strict'; frozen.zz = 1; })(); } catch (e) { console.log('frozen', e instanceof TypeError); }
const sealed = Object.preventExtensions(new Style(2));
(function () { sealed.zz = 1; })();
console.log('zz' in sealed);
let setCalls = 0;
class Guarded extends Base { constructor() { super(); this.v1 = 1; this.v2 = 2; this.v3 = 3; this.v4 = 4; this.v5 = 5; this.trap = 9; } }
Object.defineProperty(Base.prototype, 'trap', { set(v) { setCalls += v; }, get() { return 'proto trap'; }, configurable: true });
for (let i = 0; i < 5; i++) { const g = new Guarded(); console.log(g.trap, Object.keys(g).length, setCalls); }

// String literals, the inline boxing of key-table strings.
const lits = ['alpha', 'beta', '', 'a much longer literal string with spaces', 'ünïcödé', '\u{1F600}'];
const again = ['alpha', 'beta', '', 'a much longer literal string with spaces', 'ünïcödé', '\u{1F600}'];
console.log(lits.map((s, i) => s === again[i] && s.length).join());
