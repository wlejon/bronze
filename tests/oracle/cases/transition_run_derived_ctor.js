// A derived constructor's stretch of adds after `super()` is a transition run on
// the object `super()` bound `this` to. Each section warms a run and then
// hands it a receiver, a chain or a base that the one-guard form must not
// take: the writes must land as they do one by one.

function keys(o) { return Object.keys(o).join(','); }

class Base {
    constructor(flag) {
        this.base = 1;
        if (flag) this.extra = 2;
    }
}
class D extends Base {
    constructor(flag) {
        super(flag);
        this.a = 10;
        this.b = 'b';
        this.c = null;
        this.d = [];
        this.e = 0.5;
    }
}

// 1. Warm, then a base that sometimes adds one more key first.
let sum = 0;
for (let i = 0; i < 200; i++) {
    const o = new D(i % 7 === 0);
    sum += o.a + o.e + (o.extra || 0);
}
console.log('1', sum, keys(new D(false)), keys(new D(true)));

// 2. Subclasses of D (another new.target, so another start shape), and
// Reflect.construct with a newTarget whose prototype differs.
class E extends D {
    constructor() {
        super(false);
        this.f = 'f';
        this.g = 'g';
    }
}
function NT() {}
NT.prototype = { tag: 'nt' };
const viaReflect = Reflect.construct(D, [false], NT);
let es = '';
for (let i = 0; i < 50; i++) es = keys(new E());
console.log('2', es, keys(viaReflect), viaReflect.tag, Object.getPrototypeOf(viaReflect) === NT.prototype,
            viaReflect instanceof D);

// 3. A base that returns another object: a fresh literal, one already holding
// some of the keys, a frozen one, an array, and an instance made earlier.
const made = [];
class Ret {
    constructor(r) { if (r !== undefined) return r; }
}
class Q extends Ret {
    constructor(r) {
        super(r);
        this.x = 1;
        this.y = 2;
        this.z = 3;
    }
}
for (let i = 0; i < 100; i++) made.push(new Q());
const lit = new Q({ pre: 0 });
const partial = new Q({ y: 'old', w: 9 });
let frozenErr = 'none';
try { new Q(Object.freeze({ k: 1 })); } catch (e) { frozenErr = e.constructor.name; }
const arr = new Q([7]);
const again = new Q(made[0]);
console.log('3', keys(lit), keys(partial), partial.y, frozenErr, keys(arr), arr.length, arr.z, again === made[0],
            keys(again));

// 4. A setter for one of the run's keys, on the derived prototype and then on
// the base's, after the warm-up; then a read-only inherited key, which a class
// body (strict code) refuses with a TypeError.
class S0 {}
class S1 extends S0 {
    constructor() {
        super();
        this.p = 1;
        this.q = 2;
        this.r = 3;
    }
}
for (let i = 0; i < 100; i++) new S1();
const seen = [];
Object.defineProperty(S1.prototype, 'q', { set(v) { seen.push('S1.q=' + v); }, configurable: true });
const s1 = new S1();
delete S1.prototype.q;
Object.defineProperty(S0.prototype, 'r', { set(v) { seen.push('S0.r=' + v); }, configurable: true });
const s2 = new S1();
delete S0.prototype.r;
Object.defineProperty(S0.prototype, 'p', { value: 'ro', writable: false, configurable: true });
let s3 = 'none';
try { new S1(); } catch (e) { s3 = e.constructor.name; }
delete S0.prototype.p;
const s4 = new S1();
console.log('4', seen.join(' '), keys(s1), keys(s2), s3, keys(s4));

// 5. A base that returns a prototype object, and one that returns a proxy.
class P0 { constructor(r) { if (r) return r; } }
class P1 extends P0 {
    constructor(r) {
        super(r);
        this.m = 'm';
        this.n = 'n';
    }
}
for (let i = 0; i < 100; i++) new P1();
const protoObj = {};
const kid = Object.create(protoObj);
const viaProto = new P1(protoObj);
const traps = [];
const px = new Proxy({}, { set(t, k, v) { traps.push(k); t[k] = v; return true; } });
const viaPx = new P1(px);
console.log('5', viaProto === protoObj, kid.m + kid.n, keys(kid), traps.join(','), keys(viaPx));

// 6. Class fields of the derived class come first, then the constructor body.
class F0 { constructor() { this.base = 'b'; } }
class F1 extends F0 {
    field = 'f';
    constructor() {
        super();
        this.after = 'a';
        this.last = 'l';
    }
}
let fk = '';
for (let i = 0; i < 50; i++) fk = keys(new F1());
console.log('6', fk);

// 7. `this` stored into itself mid-stretch, and values made in the stretch.
class T0 {}
class T1 extends T0 {
    constructor() {
        super();
        this.one = 1;
        this.self = this;
        this.two = {};
        this.three = [];
    }
}
let tk = '';
let ok = true;
for (let i = 0; i < 50; i++) {
    const t = new T1();
    tk = keys(t);
    ok = ok && t.self === t && t.two !== t;
}
console.log('7', tk, ok);
