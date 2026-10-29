// A constructor's stretch of `this.k = v` adds runs as one guard and one shape
// store once the runtime has seen it build its shape (codegen-brass
// il_transition_run.cpp). Each block below warms a run until it is published,
// then changes one thing the guard stands for, and checks the object is the
// one the writes one by one would have made.

const out = [];
function keys(o) { return Object.keys(o).join(","); }

// 1. Six fields: the last two go to the overflow block the fast copy allocates.
class Six {
  constructor(a) { this.a = a; this.b = a + 1; this.c = a + 2; this.d = a + 3; this.e = a + 4; this.f = a + 5; }
}
let s6 = 0;
for (let i = 0; i < 40; i++) { const o = new Six(i); s6 += o.a + o.f; }
const last6 = new Six(100);
console.log("six", s6, keys(last6), last6.e, last6.f);

// 2. A setter appears on the prototype after the run is published.
function P(a, b) { this.a = a; this.b = b; }
for (let i = 0; i < 20; i++) new P(i, i);
Object.defineProperty(P.prototype, "b", { set(v) { out.push("setb" + v); }, configurable: true });
const p1 = new P(1, 2);
console.log("setter", keys(p1), Object.prototype.hasOwnProperty.call(p1, "b"), out.join(","));
delete P.prototype.b;
for (let i = 0; i < 20; i++) new P(i, i);
const p2 = new P(3, 4);
console.log("setter gone", keys(p2), p2.b);

// 3. A read-only property on the prototype: a sloppy write fails silently.
function Q(a, b) { this.a = a; this.b = b; }
for (let i = 0; i < 20; i++) new Q(i, i);
Object.defineProperty(Q.prototype, "b", { value: 9, writable: false, configurable: true });
const q1 = new Q(1, 2);
console.log("readonly", keys(q1), q1.b);

// 4. A proxy on the chain takes every write.
function X(a, b) { this.a = a; this.b = b; }
for (let i = 0; i < 20; i++) new X(i, i);
const trapped = [];
Object.setPrototypeOf(X.prototype, new Proxy({}, { set(t, k, v) { trapped.push(k + "=" + v); return true; } }));
const x1 = new X(5, 6);
console.log("proxy", "[" + keys(x1) + "]", trapped.join(","));

// 5. The same function called plainly on objects of other shapes.
function G(a, b) { this.a = a; this.b = b; }
for (let i = 0; i < 20; i++) new G(i, i);
const g1 = { b: 0 };
G.call(g1, 1, 2);
const g2 = {};
G.call(g2, 3, 4);
const g3 = Object.freeze({ z: 1 });
G.call(g3, 5, 6);
const g4 = new G(7, 8);
console.log("plain call", keys(g1), g1.a, g1.b, keys(g2), keys(g3), keys(g4), g4.b);

// 6. `this` frozen before the run.
function H(a, b) { if (a === 0) Object.freeze(this); this.a = a; this.b = b; }
for (let i = 1; i < 20; i++) new H(i, i);
const h0 = new H(0, 5);
const h1 = new H(1, 2);
console.log("frozen", "[" + keys(h0) + "]", Object.isFrozen(h0), keys(h1), h1.b);

// 7. Values that change kind under the run's double slots.
function N(a, b) { this.a = a; this.b = b; }
let ns = 0;
for (let i = 0; i < 20; i++) { const o = new N(i + 0.5, i * 2); ns += o.a + o.b; }
const n1 = new N("s", 2.5);
const n2 = new N(1.5, { k: 1 });
const n3 = new N(undefined, null);
const n4 = new N(0.25, 0.75);
console.log("kinds", ns, n1.a, n1.b, n2.a, n2.b.k, n3.a, n3.b, n4.a + n4.b);

// 8. Fields in different orders on different paths.
function C(f) { if (f) { this.x = 1; this.y = 2; } else { this.y = 3; this.x = 4; } }
for (let i = 0; i < 20; i++) { new C(true); new C(false); }
const c1 = new C(true), c2 = new C(false);
console.log("orders", keys(c1), c1.x + c1.y, keys(c2), c2.x + c2.y);

// 9. A constructor that returns an object.
function R(a) { this.a = a; this.b = a; return { z: a }; }
for (let i = 0; i < 20; i++) new R(i);
const r1 = new R(3);
console.log("returns", keys(r1), r1.z);

// 10. A base constructor's run under a subclass: its start shape alternates.
class A { constructor() { this.p = 1; this.q = 2; } }
class B extends A { constructor() { super(); this.r = 3; this.s = 4; } }
let ab = 0;
for (let i = 0; i < 200; i++) { const a = new A(); const b = new B(); ab += a.p + a.q + b.p + b.q + b.r + b.s; }
const b1 = new B();
console.log("subclass", ab, keys(new A()), keys(b1), b1 instanceof A);

// 11. Reflect.construct with another new.target.
function Other() {}
for (let i = 0; i < 20; i++) new A();
const ro = Reflect.construct(A, [], Other);
console.log("reflect", keys(ro), ro instanceof Other, ro instanceof A);

// 12. A setter on Object.prototype, then removed.
class Z { constructor(v) { this.yy = v; this.zz = v; } }
for (let i = 0; i < 20; i++) new Z(i);
const zlog = [];
Object.defineProperty(Object.prototype, "zz", { set(v) { zlog.push(v); }, configurable: true });
const z1 = new Z(7);
delete Object.prototype.zz;
const z2 = new Z(8);
console.log("object proto", keys(z1), zlog.join(","), keys(z2), z2.zz);

// 13. defineProperty between writes.
function D(a) { this.a = a; Object.defineProperty(this, "b", { value: 2, enumerable: false }); this.c = 3; this.d = 4; }
for (let i = 0; i < 20; i++) new D(i);
const d1 = new D(1);
console.log("define", keys(d1), Object.getOwnPropertyNames(d1).join(","), d1.b);

// 14. A throw part way through.
function E(a) { this.a = a; this.b = a.foo.bar; this.c = 1; this.d = 2; }
for (let i = 0; i < 20; i++) new E({ foo: { bar: i } });
let thrown = "none";
try { new E(1); } catch (err) { thrown = err instanceof TypeError ? "TypeError" : "other"; }
const e1 = new E({ foo: { bar: 5 } });
console.log("throws", thrown, keys(e1), e1.b);

// 15. A ToNumber of a parameter inside the run.
function U(a, b) { this.a = +a; this.b = b * 2; this.c = 0; }
for (let i = 0; i < 20; i++) new U(i, i);
const vo = [];
const u1 = new U({ valueOf() { vo.push("v"); return 7; } }, 1);
const u2 = new U("5", "6");
console.log("tonumber", u1.a, u1.b, vo.join(","), u2.a, u2.b, typeof u2.a);

// 16. Literals filled one key at a time, under allocation pressure.
function lit(i) { const o = {}; o.k0 = i; o.k1 = []; o.k2 = {}; o.k3 = i * 2; o.k4 = "s"; o.k5 = [i]; o.k6 = i + 0.5; return o; }
const lits = [];
for (let i = 0; i < 300; i++) { const o = lit(i); o.k1.push(i); o.k2.w = i; lits.push(o); }
let ls = 0;
for (const o of lits) ls += o.k0 + o.k1[0] + o.k2.w + o.k3 + o.k5[0] + o.k6;
console.log("literals", ls, keys(lits[299]), lits[299].k4);

// 17. A non-extensible prototype chain does not matter; a non-extensible
// receiver made by a base class does.
class Sealer { constructor() { Object.preventExtensions(this); } }
class Sealed extends Sealer { constructor() { super(); this.m = 1; this.n = 2; } }
let sealedErr = "none";
try { new Sealed(); } catch (err) { sealedErr = err instanceof TypeError ? "TypeError" : "other"; }
console.log("sealed", sealedErr);

// The same questions of class constructors, whose writes have static slots and
// so are the ones marked (a class body is strict: a failed write throws).
function threw(f) { try { f(); return "none"; } catch (err) { return err instanceof TypeError ? "TypeError" : "other"; } }

class P2 { constructor(a, b) { this.a = a; this.b = b; } }
for (let i = 0; i < 20; i++) new P2(i, i);
const out2 = [];
Object.defineProperty(P2.prototype, "b", { set(v) { out2.push("setb" + v); }, configurable: true });
const cp1 = new P2(1, 2);
delete P2.prototype.b;
const cp2 = new P2(3, 4);
console.log("class setter", keys(cp1), out2.join(","), keys(cp2), cp2.b);

class Q2 { constructor(a, b) { this.a = a; this.b = b; } }
for (let i = 0; i < 20; i++) new Q2(i, i);
Object.defineProperty(Q2.prototype, "b", { value: 9, writable: false, configurable: true });
const cq = threw(() => new Q2(1, 2));
delete Q2.prototype.b;
console.log("class readonly", cq, keys(new Q2(1, 2)));

class X2 { constructor(a, b) { this.a = a; this.b = b; } }
for (let i = 0; i < 20; i++) new X2(i, i);
const trapped2 = [];
Object.setPrototypeOf(X2.prototype, new Proxy({}, { set(t, k, v) { trapped2.push(k + "=" + v); return true; } }));
const cx = new X2(5, 6);
console.log("class proxy", "[" + keys(cx) + "]", trapped2.join(","));

class H2 { constructor(a, b) { if (a === 0) Object.freeze(this); this.a = a; this.b = b; } }
for (let i = 1; i < 20; i++) new H2(i, i);
const ch0 = threw(() => new H2(0, 5));
const ch1 = new H2(1, 2);
console.log("class frozen", ch0, keys(ch1), ch1.b);

class N2 { constructor(a, b) { this.a = a; this.b = b; } }
let ns2 = 0;
for (let i = 0; i < 20; i++) { const o = new N2(i + 0.5, i * 2); ns2 += o.a + o.b; }
const cn1 = new N2("s", 2.5), cn2 = new N2(1.5, { k: 1 }), cn3 = new N2(undefined, null), cn4 = new N2(0.25, 0.75);
console.log("class kinds", ns2, cn1.a, cn1.b, cn2.a, cn2.b.k, cn3.a, cn3.b, cn4.a + cn4.b);

class C2 { constructor(f) { if (f) { this.x = 1; this.y = 2; } else { this.y = 3; this.x = 4; } } }
for (let i = 0; i < 20; i++) { new C2(true); new C2(false); }
const cc1 = new C2(true), cc2 = new C2(false);
console.log("class orders", keys(cc1), cc1.x + cc1.y, keys(cc2), cc2.x + cc2.y);

class R2 { constructor(a) { this.a = a; this.b = a; return { z: a }; } }
for (let i = 0; i < 20; i++) new R2(i);
console.log("class returns", keys(new R2(3)));

class D2 { constructor(a) { this.a = a; Object.defineProperty(this, "b", { value: 2, enumerable: false }); this.c = 3; this.d = 4; } }
for (let i = 0; i < 20; i++) new D2(i);
const cd = new D2(1);
console.log("class define", keys(cd), Object.getOwnPropertyNames(cd).join(","));

class E2 { constructor(a) { this.a = a; this.b = a.foo.bar; this.c = 1; this.d = 2; } }
for (let i = 0; i < 20; i++) new E2({ foo: { bar: i } });
console.log("class throws", threw(() => new E2(1)), keys(new E2({ foo: { bar: 5 } })));

class U2 { constructor(a, b) { this.a = +a; this.b = b * 2; this.c = 0; } }
for (let i = 0; i < 20; i++) new U2(i, i);
const vo2 = [];
const cu1 = new U2({ valueOf() { vo2.push("v"); return 7; } }, 1);
const cu2 = new U2("5", "6");
console.log("class tonumber", cu1.a, cu1.b, vo2.join(","), cu2.a, cu2.b, typeof cu2.a);

// A key the instance already has when the run starts: a base constructor
// that defines it first, and a run whose start shape is then not its own.
class K { constructor() { this.m = 0; } }
class K2 extends K { constructor() { super(); this.n = 1; this.o = 2; } }
class K3 extends K { constructor() { super(); Object.defineProperty(this, "n", { value: 5, writable: false }); } }
for (let i = 0; i < 20; i++) new K2();
const k3 = new K3();
console.log("class prior", keys(new K2()), keys(k3), k3.n);
