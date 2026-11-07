// A method site's inline guards read way 0 from the site's METHOD ROW, a
// packed copy of way 0's shape, code, arity and epoch words that the runtime
// rewrites after every latch and the method stub rewrites when a way-1 hit
// swaps the two ways. A row that lags its site would call a function the site
// no longer holds (or the right code with the other way's env), so every
// answer here must stay the receiver's own method through: alternating shapes
// (way-1 swaps), a third and fourth shape (table answers and relatches), a
// prototype method patched mid-run (epoch), an own closure shadowing the
// inherited method (SLOT form), closures carrying different environments
// latched on alternate shapes, a class static, array and string receivers at
// a site that also meets plain objects, and sites in many functions so their
// rows share lines.
class A {
    constructor(v) { this.v = v; }
    f(x) { return this.v + x; }
    g(x) { return this.v * 2 + x; }
}
class B extends A { constructor(v) { super(v); this.extra = 1; } f(x) { return this.v - x; } }
class C extends A { constructor(v) { super(v); this.c = 2; this.d = 3; } }
class D extends B { constructor(v) { super(v); this.dd = 4; } g(x) { return x - this.v; } }

function callF(o, x) { return o.f(x); }
function callG(o, x) { return o.g(x); }
function callBoth(o, x) { return o.f(x) * 3 + o.g(x); }

function run(label, objs, reps, fn) {
    let h = 0;
    for (let r = 0; r < reps; r++) for (let i = 0; i < objs.length; i++) h = (h * 31 + fn(objs[i], r)) | 0;
    console.log(label, h);
}

const a = new A(1), b = new B(2), c = new C(3), d = new D(4);
run("mono", [a, a, a], 30, callF);
run("alternate", [a, b, a, b], 40, callF);
run("three", [a, b, c, a, b, c], 40, callF);
run("four", [a, b, c, d, d, c, b, a], 80, callBoth);
run("dominant", [d, d, d, d, d, d, d, a, b], 100, callG);

// A prototype method patched mid-run: the epoch moves, every row's latch is
// stale, and the next call must find the new method.
A.prototype.g = function (x) { return this.v * 100 + x; };
run("patched", [a, c, a, c], 20, callG);
B.prototype.f = function (x) { return -this.v - x; };
run("patched2", [a, b, c, d], 20, callBoth);

// An own closure shadowing the inherited method: the SLOT form at the site,
// then back to a receiver of the plain shape.
const own = new A(5);
own.f = function (x) { return 1000 + x; };
run("slot", [own, a, own, a], 30, callF);
const owns = [];
for (let i = 0; i < 6; i++) { const o = new C(i); o.f = (x) => i * 10 + x; owns.push(o); }
run("slots", owns, 30, callF);

// Methods closing over different environments, latched on alternating
// shapes, so the way-1 swap moves an env with its code: a row that kept the
// other way's code would run it with this way's env.
function makeClass(base, k) {
    const local = { k };
    return class extends base { constructor(v) { super(v); this["p" + k] = k; } f(x) { return local.k * 7 + this.v + x; } };
}
const K1 = makeClass(A, 1), K2 = makeClass(A, 2), K3 = makeClass(C, 3);
const ks = [new K1(1), new K2(2), new K1(3), new K2(4), new K3(5)];
run("envs", ks, 60, callF);
for (let i = 0; i < 200000; i++) { const junk = [i, { i }, "s" + i]; if (junk.length === 9) console.log("never"); }
run("envs-after-gc", ks, 60, callF);

// A class static, an array and a string at sites that meet plain objects too.
class S { static make(x) { return x * 3; } }
const holders = [S, { make: (x) => x + 1 }, S];
let sh = 0;
for (let r = 0; r < 50; r++) for (const h of holders) sh = (sh * 7 + h.make(r)) | 0;
console.log("static", sh);
function lenOf(o) { return o.indexOf(2); }
const mixed = [[1, 2, 3], "x2y", { indexOf: (x) => x + 40 }, [2], "222"];
let mh = 0;
for (let r = 0; r < 50; r++) for (const o of mixed) mh = (mh * 7 + lenOf(o)) | 0;
console.log("exotic", mh);

// Many sites in a row of small functions: neighbouring rows in one line.
function many(o, x) {
    let s = 0;
    s += o.f(x); s += o.g(x); s += o.f(x + 1); s += o.g(x + 1);
    s += o.f(x + 2); s += o.g(x + 2); s += o.f(x + 3); s += o.g(x + 3);
    return s;
}
run("many", [a, b, c, d, a, a, b, b], 50, many);
