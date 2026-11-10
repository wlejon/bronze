// Shape::lookupProperty memoizes (shape, key) for transition-tree shapes and
// arena / symbol keys. Shapes reached many ways, attributes changed in place,
// deletes into dictionary mode and keys that are run-time strings must all read
// what the object actually holds.

const sym = Symbol("memo");
function make(i) {
  const o = { a: i, b: i + 1 };
  if (i & 1) o.c = "odd";
  if (i & 2) o[sym] = "two";
  return o;
}
const objs = []; for (let i = 0; i < 8; i++) objs.push(make(i));
const probe = (o) => [o.a, o.b, o.c, o[sym], "c" in o, sym in o,
  Object.getOwnPropertyDescriptor(o, "b").writable].join(",");
for (const o of objs) console.log(probe(o));

// Change attributes: same key, new shape (or dictionary).
const w = make(3);
Object.defineProperty(w, "b", { writable: false });
w.b = 99;
console.log("nonwritable", probe(w), w.b);
Object.defineProperty(w, "a", { get() { return "getter"; } });
console.log("accessor", w.a, Object.keys(w).join(","));

// Freeze and seal keep reads right.
const fz = Object.freeze(make(1));
fz.a = 5; fz.z = 1;
console.log("frozen", probe(fz), fz.z, Object.isFrozen(fz));
const se = Object.seal(make(2)); se.a = 7; delete se.b;
console.log("sealed", probe(se));

// Delete into dictionary, then re-add.
const d = make(1); delete d.a;
console.log("deleted", d.a, "a" in d, Object.keys(d).join(","));
d.a = "back";
console.log("readded", d.a, Object.keys(d).join(","));

// Run-time string keys (heap strings) and their interned twins.
const rk = ["c", "b", "a"].map((s) => s.repeat(1));
const o3 = make(3);
console.log("runtime keys", rk.map((k) => o3[k]).join(","), rk.map((k) => k in o3).join(","));
const long = "x".repeat(40);
const lo = {}; lo[long] = 1;
console.log("long key", lo["x".repeat(40)], ("x".repeat(39) + "x") in lo, lo["x".repeat(41)]);

// Prototype chain: a key added later to the prototype appears on lookups.
class P {}
const inst = new P();
console.log("before", inst.late, "late" in inst);
P.prototype.late = "now";
console.log("after", inst.late, "late" in inst);
Object.defineProperty(P.prototype, "late", { get() { return "got"; } });
console.log("getter", inst.late);

// Numeric-string keys on plain objects.
const n = { "0": "zero", "10": "ten", "1e3": "sci" };
console.log("numeric", n[0], n["10"], n[1e3], n["1e3"], n[1000], Object.keys(n).join(","));

// Many shapes, so the memo's direct-mapped slots collide and are replaced.
let acc = 0;
const many = [];
for (let i = 0; i < 3000; i++) { const o = {}; o["f" + (i % 97)] = i; o.g = i; many.push(o); }
for (const o of many) acc += o.g + (o["f" + (o.g % 97)] === o.g ? 1 : 0) + ("g" in o ? 1 : 0);
console.log("many", acc);
