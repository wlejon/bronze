// The read stub's misses after the receiver's header check
// (__bronze_prop_get_ic, il_prop_get_fast.cpp): the full stub is handed the
// receiver boxed again from its payload, so every answer it gives there has to
// see the receiver itself. A getter's `this`, on the receiver and up the
// chain; a prototype in dictionary mode at each depth; a chain longer than the
// stub walks; a receiver whose prototype was swapped; a table entry whose
// shape matches and whose key does not; an epoch that moved.

function readV(o) { return o.v; }
function readW(o) { return o.w; }

function check(label, objs, read) {
    const out = [];
    for (let round = 0; round < 3; round++) {
        for (const o of objs) out.push(String(read(o)));
    }
    console.log(label, out.join(","));
}

// Getters, on the receiver and at each depth: `this` is the receiver.
function chainOf(depth, tag) {
    let proto = {
        get v() { return this.tag + ":" + depth; },
    };
    for (let i = 1; i < depth; i++) proto = Object.create(proto);
    const o = Object.create(proto);
    o.tag = tag;
    return o;
}
const getterObjs = [];
for (let d = 1; d <= 6; d++) getterObjs.push(chainOf(d, "g" + d));
const own = { tag: "own" };
Object.defineProperty(own, "v", { get() { return this.tag + ":0"; } });
getterObjs.push(own);
check("getters", getterObjs, readV);

// Data on a prototype that went to dictionary mode, at depths 1..5.
function dictChain(depth, tag) {
    const holder = { v: tag, junk: 1 };
    delete holder.junk;
    let proto = holder;
    for (let i = 1; i < depth; i++) proto = Object.create(proto);
    return Object.create(proto);
}
const dictObjs = [];
for (let d = 1; d <= 5; d++) dictObjs.push(dictChain(d, "d" + d));
check("dictionary", dictObjs, readV);

// Receivers whose prototype was swapped (each goes to dictionary mode).
const swapped = [];
for (let i = 0; i < 4; i++) {
    const o = { tag: "s" + i };
    Object.setPrototypeOf(o, i % 2 ? { v: "odd" } : { get v() { return "even " + this.tag; } });
    swapped.push(o);
}
check("swapped protos", swapped, readV);

// Many shapes and two keys: table entries that match the shape and not the key.
const many = [];
for (let i = 0; i < 12; i++) {
    const o = {};
    for (let k = 0; k < i; k++) o["p" + k] = k;
    if (i % 2) o.v = "v" + i;
    else o.w = "w" + i;
    many.push(o);
}
check("table v", many, readV);
check("table w", many, readW);

// The epoch moves under a cached prototype answer; then a getter replaces it.
class Base {}
Base.prototype.v = "base";
const insts = [new Base(), new Base(), new Base()];
insts[1].tag = "one";
check("before", insts, readV);
Base.prototype.v = "changed";
check("after write", insts, readV);
Object.defineProperty(Base.prototype, "v", { get() { return "getter " + (this === insts[2]); }, configurable: true });
check("after define", insts, readV);
delete Base.prototype.v;
check("after delete", insts, readV);

// Collections between reads: the boxed receiver is still the object.
const keep = [];
for (let i = 0; i < 2000; i++) {
    keep.push({ big: new Array(64).fill(i) });
    if (i % 400 === 0) check("gc " + i, getterObjs.slice(0, 3), readV);
}
console.log("kept", keep.length);
