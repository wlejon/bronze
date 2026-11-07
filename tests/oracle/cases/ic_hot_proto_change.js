// Prototype changes under warm property sites. A depth > 0 entry and an absent
// entry are answered from way 0's packed row (bronze_abi_layout.h, "the HOT
// array") plus the site's epoch; every prototype mutation must still reach the
// answer: a shadowing property added on an intermediate, a prototype method
// replaced and deleted, Object.setPrototypeOf on a receiver and on a
// prototype, an absent key that appears on the chain, and a null-prototype
// object at the same site.
class A { m() { return "A.m"; } get k() { return "A.k"; } }
class B extends A {}
class C extends B {}

function readM(o) { return typeof o.m === "function" ? o.m() : String(o.m); }
function readK(o) { return o.k; }
function readZ(o) { return o.z; }

const c = new C(), b = new B();
const out = [];
function round(label) {
    out.push(label + ": " + [readM(c), readM(b), readK(c), String(readZ(c)), String(readZ(b))].join(","));
}
for (let i = 0; i < 5; i++) round("warm" + i);
B.prototype.m = function () { return "B.m"; };
round("shadow");
A.prototype.m = function () { return "A.m2"; };
round("replace-base");
delete B.prototype.m;
round("unshadow");
A.prototype.z = "zA";
round("absent-appears");
B.prototype.z = "zB";
round("absent-shadow");
class D { m() { return "D.m"; } get k() { return "D.k"; } }
Object.setPrototypeOf(c, D.prototype);
round("setproto-recv");
Object.setPrototypeOf(B.prototype, D.prototype);
round("setproto-mid");
delete A.prototype.z;
delete B.prototype.z;
round("deleted");

const n = Object.create(null);
n.m = "own";
n.k = "nk";
let h = "";
for (let i = 0; i < 6; i++) h += readM(i % 2 ? n : c) + "/" + readK(i % 2 ? n : b) + "/" + String(readZ(n)) + ";";
out.push("null-proto: " + h);

// Many receivers of one class whose prototype gains and loses a property.
class E { constructor(i) { this.i = i; } }
const es = [];
for (let i = 0; i < 10; i++) es.push(new E(i));
function readF(o) { return o.f; }
let s = 0;
for (let r = 0; r < 30; r++) {
    if (r === 10) E.prototype.f = 3;
    if (r === 20) delete E.prototype.f;
    for (const e of es) s += (readF(e) | 0) + e.i;
}
out.push("epoch: " + s);
console.log(out.join("\n"));
