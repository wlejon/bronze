// for-in over same-shape objects reuses the key array the last enumeration of
// that shape built. Every change that can alter the keys must still be seen.

function keys(o) { const out = []; for (const k in o) out.push(k); return out.join(","); }

class Point { constructor(x, y) { this.x = x; this.y = y; } }
const pts = [];
for (let i = 0; i < 100; i++) pts.push(new Point(i, -i));
let n = 0;
for (let r = 0; r < 50; r++) for (const p of pts) for (const k in p) n += k.length + p[k];
console.log("same shape", n, keys(pts[3]));

// The object's own keys change: a new shape, a new list.
const o = { a: 1, b: 2 };
console.log("before", keys(o));
o.c = 3;
console.log("added", keys(o));
delete o.a;
console.log("deleted", keys(o));
Object.defineProperty(o, "hidden", { value: 1, enumerable: false });
console.log("non-enumerable", keys(o));

// The chain changes under an unchanged receiver shape.
const proto = { inherited: 1 };
const child = Object.create(proto);
child.own = 1;
console.log("chain", keys(child));
proto.later = 2;
console.log("proto add", keys(child));
delete proto.inherited;
console.log("proto delete", keys(child));
Object.prototype.polluted = 1;
console.log("object proto", keys({ z: 1 }));
delete Object.prototype.polluted;
console.log("restored", keys({ z: 1 }));

// Nested and re-entrant enumerations of one shape.
const pair = { p: 1, q: 2 };
const nested = [];
for (const a in pair) for (const b in pair) nested.push(a + b);
console.log("nested", nested.join(","));
function rec(obj, depth) { let s = ""; for (const k in obj) s += k + (depth ? rec(obj, depth - 1) : ""); return s; }
console.log("recursive", rec(pair, 2));

// Keys added during a loop are not part of the snapshot the loop walks.
const grow = { m: 1 };
const seen = [];
for (const k in grow) { seen.push(k); grow["n" + seen.length] = 1; }
console.log("grow", seen.join(","), keys(grow));

// More shapes than the cache holds, interleaved, across collections.
const shapes = [];
for (let i = 0; i < 300; i++) { const s = {}; s["k" + i] = i; s.common = 1; shapes.push(s); }
let total = 0;
for (let r = 0; r < 20; r++) {
  for (const s of shapes) for (const k in s) total += k.length;
  const junk = []; for (let j = 0; j < 500; j++) junk.push({ j });
}
console.log("many shapes", total);

// Arrays, strings, typed arrays and null/undefined keep their own paths.
console.log("array", keys([5, 6]), "string", keys("ab"), "typed", keys(new Int8Array(2)));
console.log("nullish", keys(null), keys(undefined));
