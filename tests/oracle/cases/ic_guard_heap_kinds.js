// Every kind of value reaching property sites that warmed on plain objects.
// A site's way-0 guard tests the receiver and compares its shape word with the
// shape the site cached; a value that is no plain object must never take a
// plain object's answer, whatever sits where a plain object keeps its shape:
// a string, an array (empty, with elements, with named properties), a
// function and a class, a typed array, an ArrayBuffer and a DataView, a
// RegExp (its lastIndex is no slot), a BigInt, a symbol, a Proxy, a Map and a
// Set, a Date, an arguments object, a bound function, a boolean and a number.
// Each is read, written and read again at the same sites, with plain objects
// in between so the sites stay warm.
class Pt { constructor(x, y) { this.x = x; this.y = y; this.lastIndex = -1; this.length = 99; this.size = 7; } }

function readX(o) { return o.x; }
function readLen(o) { return o.length; }
function readLast(o) { return o.lastIndex; }
function readSize(o) { return o.size; }
function readMissing(o) { return o.missing; }
function writeX(o, v) { o.x = v; }

const warm = [];
for (let i = 0; i < 8; i++) warm.push(new Pt(i, -i));
function heat() {
    let h = 0;
    for (let r = 0; r < 20; r++) {
        for (const p of warm) {
            writeX(p, readX(p) + 1);
            h = (h * 31 + readX(p) + readLen(p) + readLast(p) + readSize(p) + (readMissing(p) === undefined ? 1 : 2)) | 0;
        }
    }
    return h;
}

function show(v) {
    if (typeof v === "bigint") return v + "n";
    if (typeof v === "symbol") return v.toString();
    if (typeof v === "function") return "fn:" + v.name;
    if (v && typeof v === "object") return Object.prototype.toString.call(v);
    return String(v);
}

function probe(label, v) {
    const before = heat();
    let r = [];
    try { r.push(show(readX(v))); } catch (e) { r.push("throw:" + e.constructor.name); }
    try { r.push(show(readLen(v))); } catch (e) { r.push("throw:" + e.constructor.name); }
    try { r.push(show(readLast(v))); } catch (e) { r.push("throw:" + e.constructor.name); }
    try { r.push(show(readSize(v))); } catch (e) { r.push("throw:" + e.constructor.name); }
    try { r.push(show(readMissing(v))); } catch (e) { r.push("throw:" + e.constructor.name); }
    // A primitive's write is dropped in sloppy code and throws in strict;
    // either way the read after it is what is pinned.
    try { writeX(v, 41); } catch (e) {}
    try { r.push(show(readX(v))); } catch (e) { r.push("throw:" + e.constructor.name); }
    const after = heat();
    console.log(label + ": " + r.join(" ") + " | " + (before !== 0) + (after !== 0));
}

function args() { return arguments; }
const named = [1, 2, 3];
named.x = "nx";
const fnWithX = function f() {};
fnWithX.x = "fx";
class K { static x = "kx"; }
const ta = new Float64Array(4);
ta.x = "tx";
const buf = new ArrayBuffer(16);
const dv = new DataView(buf);
const re = /a/g;
re.lastIndex = 3;
const prox = new Proxy({ x: "px" }, { get(t, k) { return k === "length" ? "plen" : t[k]; } });
const m = new Map([[1, 2]]);
const st = new Set([1, 2, 3]);

probe("string", "abc");
probe("empty-array", []);
probe("array", [1, 2]);
probe("array-named", named);
probe("function", fnWithX);
probe("class", K);
probe("bound", fnWithX.bind(null));
probe("typed-array", ta);
probe("arraybuffer", buf);
probe("dataview", dv);
probe("regexp", re);
probe("bigint", 12345678901234567890n);
probe("symbol", Symbol("s"));
probe("proxy", prox);
probe("map", m);
probe("set", st);
probe("date", new Date(0));
probe("arguments", args(1, 2));
probe("boolean", true);
probe("number", 3.5);
probe("plain", { x: 1, length: 2, lastIndex: 3, size: 4 });

// The same kinds at a site that only ever saw one plain shape, in a loop.
let s = "";
const kinds = ["abc", [], [7], fnWithX, ta, buf, dv, re, 5n, prox, m, st, new Pt(1, 2)];
for (let r = 0; r < 3; r++) for (const k of kinds) s += show(readX(k)) + "," + show(readLen(k)) + ";";
console.log("loop: " + s);

// A RegExp, a typed array and an ArrayBuffer given the prototype a plain
// object was created with share that object's shape word (both take the
// prototype's root shape). The RegExp's lastIndex and the typed array's length
// are no slots of that shape, so the site's answer for the plain object (an
// absent key, an own slot) is not theirs: the guard must tell them apart by
// more than the shape word.
const sharedProto = { tag: "proto" };
const plainOnProto = Object.create(sharedProto);
plainOnProto.extra = "plain-extra";
function readLastIndex(o) { return o.lastIndex; }
function readExtra(o) { return o.extra; }
function readLength(o) { return o.length; }
let warmed = "";
for (let i = 0; i < 30; i++) warmed = String(readLastIndex(plainOnProto)) + String(readExtra(plainOnProto)) + String(readLength(plainOnProto));
const reShared = /b/g;
reShared.lastIndex = 5;
Object.setPrototypeOf(reShared, sharedProto);
reShared.extra = "re-extra";
const taShared = new Uint8Array(3);
Object.setPrototypeOf(taShared, sharedProto);
taShared.extra = "ta-extra";
const abShared = new ArrayBuffer(8);
Object.setPrototypeOf(abShared, sharedProto);
abShared.extra = "ab-extra";
let shared = warmed;
for (let i = 0; i < 4; i++) {
    for (const o of [plainOnProto, reShared, taShared, abShared]) {
        shared += "|" + String(readLastIndex(o)) + "," + String(readExtra(o)) + "," + String(readLength(o));
    }
}
console.log("shared-proto: " + shared);
