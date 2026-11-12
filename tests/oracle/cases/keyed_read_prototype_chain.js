// `o[name]` where the name is computed and the property lives up the
// prototype chain: answered from the computed-read cache without the helper
// while nothing on the chain has changed, and by the helper the moment
// something has.
class GL {
  constructor() { this.calls = 0; }
}
const P = GL.prototype;
for (const n of ["texParameteri", "texParameterf", "flush"]) {
  P[n] = function (a) { this.calls++; return n + ":" + a; };
}
P.LIMIT = 16;
P.ratio = 1.5;
P[7] = "seven";

function read(o, k) { return o[k]; }
function call(o, k, a) { return o[k](a); }
function warm(o, k) { let r; for (let i = 0; i < 50; i++) r = read(o, k); return r; }

const gl = new GL();
const out = [];
for (let i = 0; i < 40; i++) out.push(call(gl, i & 1 ? "texParameteri" : "texParameterf", i));
console.log(out[0], out[39], gl.calls, warm(gl, "LIMIT"), warm(gl, "ratio"), warm(gl, 7), warm(gl, "7"));

// The holder's value changes in place.
P.LIMIT = 32;
P.ratio = "now a string";
P.flush = function () { return "new flush"; };
console.log(warm(gl, "LIMIT"), warm(gl, "ratio"), call(gl, "flush"));

// An own property shadows the chain, then goes away again.
warm(gl, "LIMIT");
gl.LIMIT = "own";
console.log(warm(gl, "LIMIT"));
delete gl.LIMIT;
console.log(warm(gl, "LIMIT"));

// Deeper chains: each level, a shadow added in the middle, and a miss.
class A { }
class B extends A { }
class C extends B { }
class D extends C { }
class E extends D { }
class F extends E { }
A.prototype.deep = "from A";
A.prototype.mid = "A mid";
const f = new F();
console.log(warm(f, "deep"), warm(f, "mid"), warm(f, "nothing"));
C.prototype.mid = "C mid";
console.log(warm(f, "mid"), warm(new B(), "mid"));
E.prototype.deep = "from E";
console.log(warm(f, "deep"));
delete E.prototype.deep;
console.log(warm(f, "deep"));

// An accessor up the chain sees the receiver.
Object.defineProperty(A.prototype, "who", { get() { return this.constructor.name; }, configurable: true });
console.log(warm(f, "who"), warm(new B(), "who"));
Object.defineProperty(A.prototype, "who", { value: "plain now", configurable: true, writable: true });
console.log(warm(f, "who"));

// The chain itself changes: a new prototype, and a prototype that turns into
// a dictionary.
const base1 = { tag: "base1", shared: 1 };
const base2 = { tag: "base2" };
const obj = Object.create(base1);
console.log(warm(obj, "tag"), warm(obj, "shared"));
Object.setPrototypeOf(obj, base2);
console.log(warm(obj, "tag"), warm(obj, "shared"));
const dictProto = { a: 1, b: 2, c: 3 };
const child = Object.create(dictProto);
console.log(warm(child, "b"));
delete dictProto.a;
dictProto.b = "after delete";
console.log(warm(child, "b"), warm(child, "a"));
const frozenProto = Object.freeze({ fz: "frozen" });
console.log(warm(Object.create(frozenProto), "fz"));

// Many receivers of one shape, and receivers of different shapes, one site.
const many = [];
for (let i = 0; i < 6; i++) { const g = new GL(); if (i % 3 === 0) g.extra = i; if (i === 4) g.flush = () => "own flush"; many.push(g); }
console.log(many.map((g) => call(g, "flush")).join());

// A null-prototype object, a function, an array and a primitive as receivers.
const bare = Object.create(null);
bare.x = 1;
console.log(warm(bare, "toString"), typeof warm(function () { }, "call"), typeof warm([], "push"), typeof warm("s", "trim"), warm(5, "nope"));

// Symbols and Object.prototype members by computed name.
const sym = Symbol("s");
P[sym] = "by symbol";
console.log(warm(gl, sym), typeof warm(gl, "hasOwnProperty"), warm(gl, "constructor") === GL);
