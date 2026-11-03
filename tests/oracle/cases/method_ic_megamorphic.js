// A method site whose receivers rotate through more classes than its two
// ways: the call is answered from the thread's megamorphic read table, and
// every change that moves the answer must still be seen — a prototype method
// replaced mid-loop, an own property shadowing the inherited one, a
// per-instance closure, a method deeper than the stub walks, a getter where
// the method was, and a non-function where the method was.
class Base { constructor(k) { this.k = k; } hit(a, b) { return "base:" + this.k + ":" + (a + b); } }
class A extends Base { constructor() { super("A"); this.a = 1; } }
class B extends Base { constructor() { super("B"); this.b = 2; this.bb = 3; } }
class C extends Base { constructor() { super("C"); this.c = 3; } hit(a, b) { return "C:" + (a * b); } }
class D extends A { constructor() { super(); this.k = "D"; this.d = 4; } }
class E1 extends D {}
class E2 extends E1 {}
class E3 extends E2 {}
class E4 extends E3 { constructor() { super(); this.k = "E4"; } }

function call(o, a, b) { return o.hit(a, b); }

const objs = [new A(), new B(), new C(), new D(), new E4()];
function round(label) {
    const out = [];
    for (let r = 0; r < 3; r++) for (const o of objs) out.push(call(o, r, 2));
    console.log(label, out.slice(-5).join(" "));
}

for (let i = 0; i < 200; i++) for (const o of objs) call(o, i, 1);
round("warm");

Base.prototype.hit = function (a, b) { return "patched:" + this.k + ":" + (a - b); };
round("patched");

objs[1].hit = function (a, b) { return "own:" + (a + b + 100); };
round("own");

objs[3] = new D();
objs[3].hit = (a, b) => "arrow:" + a + b;
round("closure");

Object.defineProperty(A.prototype, "hit", { get() { return (a, b) => "getter:" + this.k + (a + b); }, configurable: true });
round("getter");

delete A.prototype.hit;
round("deleted");

C.prototype.hit = 42;
try { round("nonfn"); } catch (e) { console.log("nonfn", e instanceof TypeError); }

Object.setPrototypeOf(C.prototype, { hit(a, b) { return "swapped:" + a; } });
delete C.prototype.hit;
round("swapped");
