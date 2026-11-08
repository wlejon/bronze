// Reads of non-writable, non-configurable data properties up the prototype
// chain, which an inline cache answers from the entry alone (the CONSTANT
// entry, bronze_abi_layout.h). Every way the answer can change under such an
// entry is exercised after the site has cached it: a shadowing own property,
// a shadowing property on a link between receiver and holder, prototype swaps
// of the receiver and of a link, dictionary and frozen receivers and holders,
// and values the entry cannot carry (-0, fractions, past 2^32, non-numbers).

function constant(obj, key, value) {
    Object.defineProperty(obj, key, { value, writable: false, enumerable: true, configurable: false });
}

function readK(o) { return o.K; }
function readL(o) { return o.L; }
function readKeyed(o, k) { return o[k]; }
function show(label, v) {
    console.log(label, typeof v, Object.is(v, -0) ? "-0" : String(v));
}
function readMany(label, o, n) {
    let last;
    for (let i = 0; i < n; i++) last = readK(o);
    show(label, last);
}

// A holder one link up, then two links up.
class Base {}
constant(Base.prototype, "K", 0x1406);
constant(Base.prototype, "L", 4294967295);
class Mid extends Base {}
class Leaf extends Mid {}

const b = new Base();
const leaf = new Leaf();
readMany("base", b, 5);
readMany("leaf", leaf, 5);
show("L", readL(b));
show("L leaf", readL(leaf));

// Assignment is ignored in sloppy code and throws in strict code.
b.K = 1;
show("after sloppy assign", readK(b));
(function () {
    "use strict";
    try {
        b.K = 2;
        console.log("strict assign: no throw");
    } catch (e) {
        console.log("strict assign:", e instanceof TypeError);
    }
})();
show("after strict assign", readK(b));

// A redefinition must be SameValue.
try {
    constant(Base.prototype, "K", 0x1406);
    console.log("same-value redefine ok");
} catch (e) {
    console.log("same-value redefine threw");
}
try {
    constant(Base.prototype, "K", 7);
    console.log("changing redefine: no throw");
} catch (e) {
    console.log("changing redefine:", e instanceof TypeError);
}
console.log("delete:", delete Base.prototype.K);
readMany("after redefine/delete", leaf, 3);

// A shadowing own property added after the site cached the constant.
const shadowed = new Leaf();
readMany("before own shadow", shadowed, 4);
Object.defineProperty(shadowed, "K", { value: 99, writable: true, configurable: true });
readMany("own shadow", shadowed, 4);
readMany("sibling unshadowed", new Leaf(), 4);

// A shadowing property on a link between receiver and holder.
const viaMid = new Leaf();
readMany("before mid shadow", viaMid, 4);
Object.defineProperty(Mid.prototype, "K", { value: 55, writable: true, configurable: true });
readMany("mid shadow", viaMid, 4);
readMany("base after mid shadow", new Base(), 4);
delete Mid.prototype.K;
readMany("mid shadow removed", viaMid, 4);

// The receiver's prototype swapped.
const swapped = new Leaf();
readMany("before swap", swapped, 4);
const other = {};
constant(other, "K", 1234);
Object.setPrototypeOf(swapped, other);
readMany("receiver swapped", swapped, 4);
Object.setPrototypeOf(swapped, {});
readMany("receiver swapped to none", swapped, 4);

// A link's prototype swapped.
class P1 {}
class P2 {}
constant(P1.prototype, "K", 11);
constant(P2.prototype, "K", 22);
const linkProto = Object.create(P1.prototype);
const viaLink = Object.create(linkProto);
readMany("link before", viaLink, 4);
Object.setPrototypeOf(linkProto, P2.prototype);
readMany("link swapped", viaLink, 4);

// A dictionary-mode receiver (a delete puts it there), then a frozen one.
const dict = new Leaf();
dict.a = 1;
dict.b = 2;
delete dict.a;
readMany("dict receiver", dict, 4);
dict.K = 3;  // inherited non-writable: ignored
readMany("dict receiver after assign", dict, 4);
Object.defineProperty(dict, "K", { value: 77, configurable: true });
readMany("dict receiver own", dict, 4);
const frozen = Object.freeze(new Leaf());
readMany("frozen receiver", frozen, 4);

// A holder frozen after the fact: its writable properties become constants.
const holder = { K: 5, X: 1 };
const fromFrozen = Object.create(holder);
readMany("holder writable", fromFrozen, 4);
holder.K = 6;
readMany("holder rewritten", fromFrozen, 4);
Object.freeze(holder);
readMany("holder frozen", fromFrozen, 4);
holder.K = 7;  // ignored
readMany("holder frozen assign", fromFrozen, 4);

// Values the entry cannot carry stay right.
const odd = {};
constant(odd, "a", -0);
constant(odd, "b", 1.5);
constant(odd, "c", -1);
constant(odd, "d", 4294967296);
constant(odd, "e", NaN);
constant(odd, "f", "str");
constant(odd, "g", true);
constant(odd, "h", null);
constant(odd, "i", undefined);
constant(odd, "j", 2147483648);
const oddKid = Object.create(odd);
for (const k of ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"]) {
    let v;
    for (let i = 0; i < 3; i++) v = readKeyed(oddKid, k);
    show("odd " + k, v);
}
let a0;
for (let i = 0; i < 3; i++) a0 = oddKid.a;
show("odd a named", a0);
let objVal;
const objHolder = {};
constant(objHolder, "o", { tag: "obj" });
const objKid = Object.create(objHolder);
for (let i = 0; i < 3; i++) objVal = objKid.o;
console.log("object constant", objVal.tag, objVal === objHolder.o);

// One site, many receiver shapes: past the ways, into the shared table.
const shapes = [];
for (let s = 0; s < 8; s++) {
    const o = new Leaf();
    for (let p = 0; p <= s; p++) o["p" + p] = p;
    shapes.push(o);
}
let sum = 0;
for (let r = 0; r < 20; r++) for (const o of shapes) sum += readK(o);
console.log("polymorphic sum", sum);
Object.defineProperty(shapes[3], "K", { value: 1, configurable: true });
sum = 0;
for (let r = 0; r < 20; r++) for (const o of shapes) sum += readK(o);
console.log("polymorphic sum shadowed", sum);

// A keyed read site.
let ks = 0;
for (let r = 0; r < 10; r++) ks += readKeyed(leaf, r % 2 ? "K" : "L");
console.log("keyed", ks);

// A constant called as a method.
try {
    leaf.K();
    console.log("call: no throw");
} catch (e) {
    console.log("call:", e instanceof TypeError);
}

// Descriptors.
const d = Object.getOwnPropertyDescriptor(Base.prototype, "K");
console.log("descriptor", d.value, d.writable, d.enumerable, d.configurable);

// Primitive receivers, through Object.prototype last (it pollutes everything).
constant(Object.prototype, "ZQ", 31);
let pz = 0;
for (let i = 0; i < 4; i++) pz += (5).ZQ + "s".ZQ + ({}).ZQ;
console.log("primitive", (5).ZQ, "s".ZQ, ({}).ZQ, true.ZQ, pz);
