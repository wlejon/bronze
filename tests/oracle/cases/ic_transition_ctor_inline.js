// Writes in a class constructor that ADD a property take an inline shape
// transition on the write cache's miss (il_ic_lowering.cpp
// emit_transition_arm): from the receiver's shape to the cached child, then
// the store. Each case is a way the cached transition could stop being the
// one the write would make: a different receiver, a setter or read-only
// property on the prototype chain, a prototype changed after the fill,
// dictionary and non-extensible receivers, a Proxy on the chain, exotic
// receivers, overflow growth, and a collection between the stores.

class P { constructor(x, y) { this.x = x; this.y = y; this.z = x + y; } }
const ps = []; for (let i = 0; i < 100; i++) ps.push(new P(i, i * 0.5));
console.log(ps[99].x, ps[99].y, ps[99].z, Object.keys(ps[0]).join());

// Many fields: past the inline slots and past the first overflow block.
class Wide { constructor(n) { for (let i = 0; i < n; i++) this["f" + i] = i; this.a = 1; this.b = 2; this.c = 3; this.d = 4; this.e = 5; this.f = 6; this.g = 7; } }
let w; for (let i = 0; i < 50; i++) w = new Wide(i % 3 ? 0 : 40);
console.log(Object.keys(w).length, w.a + w.g, new Wide(40).f39, new Wide(0).g);

// Doubles and references mixed at the same site, with garbage made between
// the stores (BRONZE_GC_STRESS collects at each allocation).
class Mix { constructor(i) { this.n = i % 2 ? i + 0.5 : i; this.o = { i }; this.s = "s" + i; this.arr = [i, i]; this.t = this.o.i * 2; } }
let mx = 0; for (let i = 0; i < 300; i++) { const m = new Mix(i); mx += m.n + m.o.i + m.s.length + m.arr[1] + m.t; }
console.log(mx);

// A setter on the prototype, defined after the transition has been cached:
// the write must call it, not add an own property.
class S { constructor(v) { this.v = v; this.w = v; } }
for (let i = 0; i < 20; i++) new S(i);
const seen = [];
Object.defineProperty(S.prototype, "w", { set(x) { seen.push(x); }, get() { return "proto"; }, configurable: true });
const s1 = new S(5);
console.log(Object.keys(s1).join(), s1.w, seen.join());
delete S.prototype.w;
const s2 = new S(6); console.log(Object.keys(s2).join(), s2.w);

// A read-only property on the prototype: the (strict) constructor throws.
class R { constructor() { this.a = 1; this.b = 2; } }
for (let i = 0; i < 20; i++) new R();
Object.defineProperty(R.prototype, "b", { value: 0, writable: false, configurable: true });
try { new R(); console.log("no throw"); } catch (e) { console.log(e.constructor.name); }
delete R.prototype.b; console.log(new R().b);

// The prototype's own prototype changed after the fill, to one with a setter.
class Q { constructor() { this.k = 1; this.j = 2; } }
for (let i = 0; i < 20; i++) new Q();
Object.setPrototypeOf(Q.prototype, { set j(v) { console.log("chain setter", v); } });
const q = new Q(); console.log(Object.keys(q).join(), q.j);

// A Proxy on the chain: the write reaches its set trap.
class X { constructor() { this.p = 1; this.r = 2; } }
for (let i = 0; i < 20; i++) new X();
const trap = new Proxy({}, { set(t, k, v, recv) { console.log("trap", String(k), v); return Reflect.defineProperty(recv, k, { value: v * 10, writable: true, enumerable: true, configurable: true }); } });
function NT() {} NT.prototype = trap;
const xp = Reflect.construct(X, [], NT);
console.log(xp.p, xp.r);
Object.setPrototypeOf(X.prototype, trap);
const xq = new X(); console.log(xq.p, xq.r);

// The receiver becomes non-extensible, frozen, or a dictionary part way through.
class F { constructor(mode) { this.a = 1; if (mode === 1) Object.preventExtensions(this); if (mode === 2) Object.freeze(this); if (mode === 3) { delete this.a; } this.b = 2; this.a = 3; } }
for (let i = 0; i < 20; i++) new F(0);
for (const mode of [1, 2, 3, 0]) { try { const f = new F(mode); console.log(mode, JSON.stringify(f), Object.isExtensible(f)); } catch (e) { console.log(mode, e.constructor.name); } }

// An instance made another object's prototype part way through: the shapes of
// prototypes are invalidated, not shared.
let child;
class U { constructor(make) { this.a = 1; if (make) child = Object.create(this); this.b = 2; } }
for (let i = 0; i < 20; i++) new U(false);
const u = new U(true); console.log(child.a, child.b, Object.keys(child).length);
u.a = 9; console.log(child.a);

// Exotic receivers: an Array and a String wrapper.
class A2 extends Array { constructor() { super(); this.tag = "a"; this.length = 3; } }
for (let i = 0; i < 20; i++) new A2();
const a2 = new A2(); console.log(a2.length, a2.tag, Array.isArray(a2));
// (`extends String` is refused by bronze outright, so a constructor's `this`
// is never a String wrapper; a class method's can be.)
class Str { constructor() { this.extra = 1; } static setLen(w) { "use strict"; w.extra = 2; w.length = 9; } }
for (let i = 0; i < 20; i++) new Str();
const st = new String("abc");
try { Str.setLen(st); console.log("set"); } catch (e) { console.log(e.constructor.name, st.length, st.extra, String(st)); }

// Subclasses: one base constructor's writes on receivers of different shapes.
class B0 { constructor() { this.m = 1; this.n = 2; } }
class B1 extends B0 { constructor() { super(); this.o = 3; } }
class B2 extends B0 { get n() { return "getter"; } set n(v) { this._n = v; } }
const bs = []; for (let i = 0; i < 30; i++) bs.push(i % 3 === 0 ? new B0() : i % 3 === 1 ? new B1() : new B2());
console.log(bs.slice(0, 3).map((b) => JSON.stringify(b) + ":" + b.n).join(" "));

// An own accessor defined on the instance between two writes to it.
class D { constructor(def) { this.q = 1; if (def) Object.defineProperty(this, "h", { set(v) { this._h = v + 100; }, enumerable: true, configurable: true }); this.h = 5; } }
for (let i = 0; i < 20; i++) new D(false);
console.log(JSON.stringify(new D(true)), JSON.stringify(new D(false)));
