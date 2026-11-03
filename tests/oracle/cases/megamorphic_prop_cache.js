// Read and write sites that see more shapes than their ways hold answer from
// the thread's (shape, key) table. Every way an entry can stop being true has
// to reach the full lookup: prototype adds, deletes and swaps, accessors
// defined over data, non-writable and frozen objects, and adds on a store.
const names = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"];
function make(i) {
    const o = { id: i, val: i * 2 };
    for (let j = 0; j <= i; j++) o[names[j]] = j;
    return o;
}
const objs = [];
for (let i = 0; i < 12; i++) objs.push(make(i));

function readVal(list) { let s = 0; for (const o of list) s += o.val; return s; }
function readInherited(list) { let s = ""; for (const o of list) s += String(o.extra); return s; }
function writeVal(list, v) { for (const o of list) o.val = v; }

for (let r = 0; r < 50; r++) readVal(objs);
console.log("read", readVal(objs));
writeVal(objs, 3);
for (let r = 0; r < 50; r++) writeVal(objs, r);
console.log("write", readVal(objs));

// Absent everywhere, then put on Object.prototype, then removed.
console.log("absent", readInherited(objs).length);
Object.prototype.extra = "x";
console.log("proto add", readInherited(objs));
delete Object.prototype.extra;
console.log("proto del", readInherited(objs).slice(0, 9));

// A getter on the prototype chain shadowed by nothing own: reads see it.
const proto = { get extra() { return "g" + this.id % 10; } };
const withProto = objs.map((o) => Object.setPrototypeOf({ ...o }, proto));
for (let r = 0; r < 20; r++) readInherited(withProto);
console.log("getter", readInherited(withProto));
Object.defineProperty(proto, "extra", { value: "d", configurable: true });
console.log("redefined", readInherited(withProto));

// A setter on the prototype for a key the objects do not own.
let setCount = 0;
const sproto = { set tag(v) { setCount += v; } };
const tagged = objs.map((o) => Object.setPrototypeOf({ ...o }, sproto));
function writeTag(list) { for (const o of list) o.tag = 1; }
for (let r = 0; r < 10; r++) writeTag(tagged);
console.log("setter", setCount, Object.keys(tagged[3]).includes("tag"));

// Non-writable and frozen: the cached store must not reach them.
writeVal(objs, 5);
Object.defineProperty(objs[4], "val", { writable: false });
Object.freeze(objs[7]);
writeVal(objs, 9);
console.log("nonwritable", objs.map((o) => o.val).join(","));
"use strict";
function writeStrict(list) { "use strict"; let n = 0; for (const o of list) { try { o.val = 1; } catch (e) { n++; } } return n; }
console.log("strict refusals", writeStrict(objs));

// Data turned into an accessor on the same object, after many cached reads.
const acc = objs.map((o, i) => ({ ...o }));
for (let r = 0; r < 20; r++) readVal(acc);
Object.defineProperty(acc[2], "val", { get() { return 1000; } });
console.log("accessor", readVal(acc));

// Deletes give the object a new shape; the cached slot must not be read.
const del = objs.map((o) => ({ ...o }));
for (let r = 0; r < 20; r++) readVal(del);
for (const o of del) delete o.val;
console.log("deleted", readVal(del));
for (let i = 0; i < del.length; i++) del[i].val = i;
console.log("re-added", readVal(del));

// Many more shapes than the table can tell apart, with GC churn between.
const many = [];
for (let i = 0; i < 3000; i++) { const o = { val: i }; o["p" + (i % 600)] = i; many.push(o); }
let total = 0;
for (let r = 0; r < 5; r++) { total += readVal(many); writeVal(many, r); const junk = []; for (let k = 0; k < 2000; k++) junk.push({ k }); }
console.log("many", total, readVal(many));
