// A method site keeps what its inline cache latched (bronze_abi.h, the
// METHOD-CALL site contract) and compiled code may enter that latched code
// directly; everything that changes which function `o.m` names must reach
// the next call: the method replaced on the prototype, shadowed by an own
// property, deleted back to an inherited one, the prototype swapped, the
// receiver's slot rewritten with another function, a getter that returns a
// different function each time, an Array method shadowed on one array and
// unshadowed again, a top-level binding rebound between calls, and methods
// added to a primitive's prototype.

class A { m() { return "A.m"; } }
class B extends A { }
const a = new A(), b = new B();
function call(o) { return o.m(); }
const log = [];
function step(label, o) { let r; for (let i = 0; i < 30; i++) r = call(o); log.push(label + "=" + r); }

step("start", a);
A.prototype.m = function () { return "A.m2"; };
step("proto-replaced", a);
a.m = function () { return "own"; };
step("own-shadow", a);
delete a.m;
step("own-deleted", a);
step("inherited", b);
B.prototype.m = function () { return "B.m"; };
step("sub-shadow", b);
Object.setPrototypeOf(b, { m() { return "swapped"; } });
step("proto-swapped", b);
Object.defineProperty(A.prototype, "m", { get() { return function () { return "from-getter"; }; }, configurable: true });
step("getter", a);
let flip = 0;
Object.defineProperty(A.prototype, "m", { get() { flip++; return flip % 2 ? () => "odd" : () => "even"; }, configurable: true });
const seen = []; for (let i = 0; i < 6; i++) seen.push(call(a)); log.push("getter-each=" + seen.join(","));
console.log(log.join("\n"));

// The receiver's own slot: same shape, a different function each time.
const fns = [function () { return "f0"; }, function () { return "f1"; }, () => "f2", function () { return "f3"; }];
const holder = { m: fns[0] };
const slots = [];
for (let i = 0; i < 40; i++) { holder.m = fns[i % 4]; slots.push(call(holder)); }
console.log(slots.slice(0, 8).join(","), slots.slice(32).join(","));
// A slot that stops holding a function.
holder.m = 3;
try { call(holder); } catch (e) { console.log("slot-number", e.constructor.name); }

// Array methods at one site: the builtin, an own shadow on one array, and
// the builtin again once the shadow is deleted. (The builtin prototypes are
// locked in bronze, so replacing Array.prototype.push is not a case here.)
function push(arr, v) { return arr.push(v); }
const xs = [], ys = [];
for (let i = 0; i < 30; i++) push(xs, i);
ys.push = function (v) { return "own-push:" + v; };
console.log(push(xs, 99), push(ys, 1), xs.length, ys.length);
delete ys.push;
console.log(push(xs, 6), push(ys, 2), xs.length, ys.length);

// A top-level binding rebound to another function between calls from one
// call site.
let g = function () { return "g1"; };
function callG() { return g(); }
const gs = [];
for (let i = 0; i < 30; i++) { if (i === 10) g = function () { return "g2"; }; if (i === 20) g = () => "g3"; gs.push(callG()); }
console.log(gs[9], gs[10], gs[19], gs[20], gs[29]);

// A primitive receiver: a method added to its prototype, called in a loop.
Number.prototype.twice = function () { return this * 2; };
String.prototype.shout = function () { return this.toUpperCase() + "!"; };
let prim = 0, str = "";
for (let i = 0; i < 30; i++) { prim += (i).twice(); str = ("x" + i).shout(); }
console.log(prim, str);
