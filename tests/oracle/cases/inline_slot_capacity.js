// Objects sized from their predicted layout: a class's instances, a
// literal's object and a class prototype get as many inline slots as the
// compiler predicts (up to 16), the rest of their properties numbered from the
// overflow split. Every path that names a slot has to agree on where a key
// lives: construction, inline caches, static slots, transition runs, deletes,
// defineProperty, dictionary mode, freezing, proxies and Reflect.construct.

function show(label, o) {
    const parts = [];
    for (const k of Object.keys(o)) parts.push(k + "=" + String(o[k]));
    console.log(label, parts.join(","));
}

// Many fields, more than the old four inline slots and fewer than the split.
class Body {
    constructor(x) {
        this.a = x; this.b = x + 1; this.c = x + 2; this.d = x + 3;
        this.e = x + 4; this.f = x + 5; this.g = x + 6; this.h = x + 7;
    }
    sum() { return this.a + this.b + this.c + this.d + this.e + this.f + this.g + this.h; }
}

// A subclass adding fields on top of the base's eight.
class Moving extends Body {
    constructor(x) {
        super(x);
        this.vx = x * 2; this.vy = x * 3; this.vz = x * 4;
    }
    speed() { return this.vx + this.vy + this.vz; }
}

// More fields than the split: the tail lives in the overflow block.
class Wide {
    constructor() {
        this.f0 = 0; this.f1 = 1; this.f2 = 2; this.f3 = 3; this.f4 = 4; this.f5 = 5;
        this.f6 = 6; this.f7 = 7; this.f8 = 8; this.f9 = 9; this.f10 = 10; this.f11 = 11;
        this.f12 = 12; this.f13 = 13; this.f14 = 14; this.f15 = 15; this.f16 = 16;
        this.f17 = 17; this.f18 = 18; this.f19 = 19;
    }
    total() {
        let t = 0;
        for (let i = 0; i < 20; i++) t += this["f" + i];
        return t;
    }
}

let acc = 0;
for (let i = 0; i < 200; i++) {
    const b = new Body(i);
    acc += b.sum();
    const m = new Moving(i);
    acc += m.sum() + m.speed();
}
console.log("bodies", acc);
show("body", new Body(1));
show("moving", new Moving(2));
const w = new Wide();
console.log("wide", w.total(), w.f0, w.f15, w.f16, w.f19, Object.keys(w).length);

// Outgrowing the prediction: more properties than the class installs.
const grown = new Body(10);
for (let i = 0; i < 24; i++) grown["extra" + i] = i * i;
let gs = 0;
for (let i = 0; i < 24; i++) gs += grown["extra" + i];
console.log("grown", grown.sum(), gs, Object.keys(grown).length, grown.extra23, grown.h);

// The same read site meeting instances of several capacities.
function readE(o) { return o.e; }
const mix = [new Body(1), new Moving(1), { a: 1, b: 2, c: 3, d: 4, e: 50 }, { e: 60 }, grown];
let me = 0;
for (let r = 0; r < 50; r++) for (const o of mix) me += readE(o);
console.log("mixed reads", me);

// Literals of each size, and an empty one filled in a run.
const small = { x: 1, y: 2 };
const lit = { p0: 0, p1: 1, p2: 2, p3: 3, p4: 4, p5: 5, p6: 6, p7: 7, p8: 8 };
const big = {};
for (let i = 0; i < 20; i++) big["k" + i] = i;
const huge = {
    q0: 0, q1: 1, q2: 2, q3: 3, q4: 4, q5: 5, q6: 6, q7: 7, q8: 8, q9: 9,
    q10: 10, q11: 11, q12: 12, q13: 13, q14: 14, q15: 15, q16: 16, q17: 17, q18: 18,
};
function filled(v) {
    const o = {};
    o.a = v; o.b = v + 1; o.c = v + 2; o.d = v + 3; o.e = v + 4; o.f = v + 5; o.g = v + 6;
    return o;
}
let fs = 0;
for (let i = 0; i < 100; i++) { const o = filled(i); fs += o.a + o.d + o.g; }
show("small", small);
show("lit", lit);
console.log("big", big.k0, big.k5, big.k19, Object.keys(big).length);
console.log("huge", huge.q0, huge.q15, huge.q16, huge.q18, Object.keys(huge).length);
console.log("filled", fs);
show("filled one", filled(3));
lit.p9 = 9; lit.p10 = 10;
show("lit grown", lit);

// Accessors in a literal: two slots each.
const acc2 = {
    a: 1, b: 2, c: 3,
    get g() { return this.a * 10; },
    set g(v) { this.a = v; },
    d: 4, e: 5,
};
acc2.g = 7;
console.log("accessor literal", acc2.g, acc2.d, acc2.e, Object.keys(acc2).join(","));

// Deletes: a key out of the middle, then new keys after it.
const del = new Body(100);
delete del.c;
delete del.f;
del.c = "back";
del.z = "z";
show("deleted", del);
console.log("deleted sum", del.sum());
const litDel = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6 };
delete litDel.b;
litDel.g = 7;
for (let i = 0; i < 10; i++) litDel["n" + i] = i;
delete litDel.n3;
show("literal deleted", litDel);

// defineProperty: data and accessor properties added to sized objects.
const dp = new Moving(5);
Object.defineProperty(dp, "hidden", { value: 42, enumerable: false, writable: false });
Object.defineProperty(dp, "twice", { get() { return this.a * 2; }, enumerable: true, configurable: true });
Object.defineProperty(dp, "a", { value: 99 });
console.log("defined", dp.hidden, dp.twice, dp.a, Object.keys(dp).length);
const dpLit = { a: 1, b: 2, c: 3, d: 4, e: 5 };
Object.defineProperty(dpLit, "x", { value: "x", enumerable: true });
Object.defineProperty(dpLit, "c", { get() { return "got"; }, enumerable: true });
show("defined literal", dpLit);

// Dictionary mode: many keys added and deleted.
const dict = new Body(0);
for (let i = 0; i < 300; i++) dict["d" + i] = i;
for (let i = 0; i < 300; i += 2) delete dict["d" + i];
dict.after = "after";
let ds = 0;
for (let i = 1; i < 300; i += 2) ds += dict["d" + i];
console.log("dictionary", ds, dict.a, dict.h, dict.after, Object.keys(dict).length);

// Frozen and sealed sized objects.
const frozen = Object.freeze(new Moving(3));
try { "use strict"; frozen.a = 1; } catch (e) { console.log("frozen throws", e.constructor.name); }
console.log("frozen", frozen.a, frozen.vz, Object.isFrozen(frozen));
const sealed = Object.seal({ a: 1, b: 2, c: 3, d: 4, e: 5, f: 6 });
sealed.f = 60;
sealed.g = 70;
show("sealed", sealed);

// Proxies over sized objects, and a proxy as a class's prototype.
const target = new Body(7);
const prox = new Proxy(target, {
    get(t, k, r) { return k === "e" ? "trapped" : Reflect.get(t, k, r); },
});
console.log("proxy", prox.a, prox.e, prox.h, target.e);
function ProxBase() {}
ProxBase.prototype = new Proxy({}, { get(t, k) { return typeof k === "string" ? "p:" + k : undefined; } });
class OnProxy extends ProxBase {
    constructor() { super(); this.a = 1; this.b = 2; this.c = 3; this.d = 4; this.e = 5; this.f = 6; }
}
const op = new OnProxy();
console.log("on proxy", op.a, op.f, op.missing);

// Reflect.construct with a new.target of another capacity, including an
// exotic base whose instances keep internal slots.
const rc = Reflect.construct(Body, [3], Moving);
console.log("reflect", rc instanceof Moving, rc.sum(), rc.vx);
class MyMap extends Map {
    constructor() { super(); this.a = 1; this.b = 2; this.c = 3; this.d = 4; this.e = 5; this.f = 6; }
}
const mm = new MyMap();
mm.set("k", "v");
console.log("map subclass", mm.get("k"), mm.size, mm.a, mm.f);
const viaReflect = Reflect.construct(Map, [], Body);
Map.prototype.set.call(viaReflect, "q", 1);
viaReflect.a = 1; viaReflect.b = 2; viaReflect.c = 3; viaReflect.d = 4; viaReflect.e = 5; viaReflect.f = 6;
console.log("map via reflect", viaReflect instanceof Body, Map.prototype.get.call(viaReflect, "q"), viaReflect.f);

// Changing a sized object's prototype keeps its slots.
const sp = new Body(4);
Object.setPrototypeOf(sp, { extra() { return "proto"; } });
sp.added = "added";
console.log("set proto", sp.a, sp.h, sp.added, sp.extra());
const protoLit = { __proto__: { inherited: "yes" }, a: 1, b: 2, c: 3, d: 4, e: 5 };
console.log("proto literal", protoLit.inherited, protoLit.e, Object.keys(protoLit).length);

// Functions as constructors, and prototypes holding many methods.
function Old(x) { this.a = x; this.b = x; this.c = x; this.d = x; this.e = x; this.f = x; }
Old.prototype.m0 = function () { return this.a + this.f; };
for (let i = 1; i < 20; i++) Old.prototype["m" + i] = function () { return i; };
const old = new Old(3);
console.log("function ctor", old.m0(), old.m7(), old.m19(), old.f);
class ManyMethods {
    m0() { return 0; } m1() { return 1; } m2() { return 2; } m3() { return 3; } m4() { return 4; }
    m5() { return 5; } m6() { return 6; } m7() { return 7; } m8() { return 8; } m9() { return 9; }
    m10() { return 10; } m11() { return 11; } m12() { return 12; } m13() { return 13; }
    m14() { return 14; } m15() { return 15; } m16() { return 16; } m17() { return 17; }
}
const mmeth = new ManyMethods();
let ms = 0;
for (let i = 0; i < 18; i++) ms += mmeth["m" + i]();
console.log("many methods", ms, mmeth.m17(), mmeth.m3());
ManyMethods.prototype.late = function () { return "late"; };
console.log("late method", mmeth.late());

// Spread, Object.assign and JSON round trips of sized objects.
const spread = { ...new Moving(1), extra: 1 };
show("spread", spread);
const assigned = Object.assign(new Body(0), { z1: 1, z2: 2 }, lit);
console.log("assigned", Object.keys(assigned).length, assigned.p8, assigned.z2, assigned.h);
console.log("json", JSON.stringify(new Moving(1)));
const parsed = JSON.parse(JSON.stringify(huge));
console.log("parsed", parsed.q18, Object.keys(parsed).length);

// Survives collections: many sized objects kept alive across allocation.
const keep = [];
for (let i = 0; i < 2000; i++) {
    keep.push(i % 3 === 0 ? new Moving(i) : i % 3 === 1 ? filled(i) : new Wide());
}
let ks = 0;
for (const o of keep) ks += o instanceof Moving ? o.speed() : o instanceof Wide ? o.f19 : o.g;
console.log("kept", ks);
