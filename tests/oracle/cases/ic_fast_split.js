// The read stub's call-free hit paths (__bronze_prop_get_ic, which hands the
// rest to __bronze_prop_get_slow) and the method site's inline way-0 DIRECT
// entry, on every path: own data through way 0,
// the other ways and the megamorphic table; an absent key, then the same key
// appearing on the prototype (the epoch moves); prototype data; getters that
// allocate enough to collect, that throw, and that are defined on the
// receiver itself; method calls monomorphic, rotating, replaced and shadowed;
// a method stored in an own slot; a non-function and a non-object receiver;
// calls inside try blocks. References held across every read are checked
// after many collections.

function makeShapes(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const o = {};
        for (let k = 0; k < i; k++) o["pad" + k] = k;
        o.x = i * 10;
        o.tag = "s" + i;
        out.push(o);
    }
    return out;
}

function readX(o) { return o.x; }
function readMaybe(o) { return o.missing; }

// Own data: one shape (way 0), four (all ways), seven (megamorphic).
for (const n of [1, 4, 7]) {
    const objs = makeShapes(n);
    let sum = 0;
    for (let r = 0; r < 2000; r++) {
        for (const o of objs) sum += readX(o);
    }
    console.log("own", n, sum);
}

// Absent, then present on the prototype, then present on the receiver.
class Base { constructor() { this.a = 1; } }
const bases = [];
for (let i = 0; i < 50; i++) bases.push(new Base());
let undef = 0;
for (let r = 0; r < 200; r++) for (const b of bases) if (readMaybe(b) === undefined) undef++;
console.log("absent", undef);
Base.prototype.missing = "proto";
let seen = "";
for (let r = 0; r < 3; r++) seen += readMaybe(bases[r]) + ",";
console.log("now", seen);
bases[1].missing = "own";
seen = "";
for (let r = 0; r < 3; r++) seen += readMaybe(bases[r]) + ",";
console.log("then", seen);
delete Base.prototype.missing;
seen = "";
for (let r = 0; r < 3; r++) seen += readMaybe(bases[r]) + ",";
console.log("after delete", seen);

// Prototype data, several links up, and a prototype value that changes.
class A { }
A.prototype.kind = "A";
class B extends A { }
class C extends B { }
class D extends C { }
function kindOf(o) { return o.kind; }
const ds = [new D(), new C(), new B(), new A()];
let kinds = "";
for (let r = 0; r < 500; r++) for (const d of ds) kinds = kindOf(d);
console.log("kind", kinds, kindOf(ds[0]));
A.prototype.kind = "A2";
console.log("kind2", kindOf(ds[0]), kindOf(ds[3]));
C.prototype.kind = "C";
console.log("kind3", kindOf(ds[0]), kindOf(ds[1]), kindOf(ds[2]));

// Getters: allocating (collections inside the read), throwing, own accessor.
let made = 0;
class G {
    constructor(i) { this.i = i; }
    get heavy() {
        let junk = [];
        for (let k = 0; k < 200; k++) junk.push({ k, s: "x" + k });
        made += junk.length;
        return this.i + junk[junk.length - 1].k;
    }
    get bad() { throw new Error("bad getter " + this.i); }
}
function heavyOf(g) { return g.heavy; }
const keep = [];
for (let i = 0; i < 40; i++) keep.push({ id: i, pair: [i, "p" + i] });
let hsum = 0;
for (let r = 0; r < 100; r++) {
    const g = new G(r);
    const held = keep[r % 40];
    hsum += heavyOf(g);
    if (held.pair[1] !== "p" + held.id) throw new Error("held reference broken");
}
console.log("heavy", hsum, made);
function badOf(g) { return g.bad; }
let caught = 0;
for (let r = 0; r < 30; r++) {
    try { badOf(new G(r)); } catch (e) { if (e.message === "bad getter " + r) caught++; }
}
console.log("caught", caught);
const ownAcc = {};
Object.defineProperty(ownAcc, "v", { get() { return 42; }, configurable: true });
function vOf(o) { return o.v; }
let vs = 0;
for (let r = 0; r < 300; r++) vs += vOf(ownAcc);
console.log("own accessor", vs);

// Method calls: monomorphic, rotating over shapes, replaced, shadowed.
class P {
    constructor(x) { this.x = x; }
    get2() { return this.x * 2; }
}
class Q extends P { get2() { return this.x * 3; } }
class R { constructor(x) { this.y = x; this.x = x; } get2() { return this.x + 1000; } }
function call2(o) { return o.get2(); }
const ps = [];
for (let i = 0; i < 20; i++) ps.push(new P(i));
let msum = 0;
for (let r = 0; r < 300; r++) for (const p of ps) msum += call2(p);
console.log("mono", msum);
const mixed = [new P(1), new Q(1), new R(1), { x: 5, get2() { return -1; } }, new P(2), new Q(2)];
msum = 0;
for (let r = 0; r < 300; r++) for (const m of mixed) msum += call2(m);
console.log("mixed", msum);
P.prototype.get2 = function () { return this.x * 100; };
msum = 0;
for (const p of ps) msum += call2(p);
console.log("replaced", msum);
ps[3].get2 = () => 7;
msum = 0;
for (const p of ps) msum += call2(p);
console.log("shadowed", msum);

// A method held in an own slot, then a non-function, then a non-object.
const holder = { f: function () { return this.k; }, k: 9 };
function callF(o) { return o.f(); }
let fs = 0;
for (let r = 0; r < 300; r++) fs += callF(holder);
console.log("slot", fs);
holder.f = 3;
try { callF(holder); } catch (e) { console.log("not callable", e instanceof TypeError); }
try { callF(undefined); } catch (e) { console.log("no receiver", e instanceof TypeError); }
try { readX(null); } catch (e) { console.log("null read", e instanceof TypeError); }

// Method calls in a try block, and a throwing method.
class T { m(i) { if (i === 77) throw new RangeError("m" + i); return i; } }
const t = new T();
let tsum = 0;
let rangeErrors = 0;
for (let i = 0; i < 100; i++) {
    try { tsum += t.m(i); } catch (e) { if (e instanceof RangeError) rangeErrors++; }
}
console.log("try", tsum, rangeErrors);

// Reads and calls interleaved with allocation, holding references across.
class V { constructor(i) { this.i = i; this.n = { v: i }; } val() { return this.n.v; } }
let vsum = 0;
const vs2 = [];
for (let i = 0; i < 3000; i++) {
    const a = new V(i);
    const b = { pad: i, w: new V(i + 1) };
    vs2.push([a, b]);
    if (vs2.length > 64) vs2.shift();
    vsum += a.val() + b.w.val() + a.n.v + b.w.i;
}
console.log("interleaved", vsum, vs2[0][0].i, vs2[63][1].w.n.v);
