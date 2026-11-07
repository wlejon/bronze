// One guard point testing one receiver for several identity cells (a shape
// group): the group's cell must answer only for shapes every member site was
// verified on, across shape changes between calls, two shapes at one site,
// dictionary mode, accessors, non-writable fields and GC.
function readAll(q) { return q.x + q.y * 2 + q.z * 3 + q.w * 4; }
function writeAll(t, q) { t.a = q.x; t.b = q.y; t.c = q.z; t.d = q.w; return t; }
function copyMix(q, r) { return q.x * r.y - q.y * r.x + q.z * r.w - q.w * r.z; }

const out = [];
function lit(x, y, z, w) { return { x, y, z, w }; }
let s = 0;
for (let i = 0; i < 3000; ++i) s += readAll(lit(i, 1, 2, 3));
out.push("warm " + s);

// Same keys in another order: a different shape, other slots.
out.push("order " + readAll({ w: 4, z: 3, y: 2, x: 1 }));
out.push("order " + readAll({ y: 2, x: 1, w: 4, z: 3 }));
// Alternate the two shapes at the one site.
s = 0;
for (let i = 0; i < 2000; ++i) s += readAll(i & 1 ? lit(1, 2, 3, i) : { w: i, z: 3, y: 2, x: 1 });
out.push("alt " + s);
// A missing member, an extra field, a prototype field.
out.push("missing " + readAll({ x: 1, y: 2, z: 3 }));
const extra = lit(1, 2, 3, 4); extra.v = 9; out.push("extra " + readAll(extra));
const proto = Object.create({ w: 40 }); proto.x = 1; proto.y = 2; proto.z = 3;
out.push("proto " + readAll(proto));
// An accessor in a member's slot.
const acc = lit(1, 2, 3, 4);
let gets = 0;
Object.defineProperty(acc, "z", { get() { ++gets; return 30; }, configurable: true });
out.push("getter " + readAll(acc) + " " + gets);
// Deleted then re-added field (dictionary or re-shaped).
const del = lit(1, 2, 3, 4); delete del.y; del.y = 20; out.push("readd " + readAll(del));
const del2 = lit(1, 2, 3, 4); delete del2.w; out.push("deleted " + readAll(del2));
for (let i = 0; i < 500; ++i) s += readAll(lit(i, 2, 3, 4));
out.push("again " + s);

// Stores: a frozen or non-writable target must not take the group's path.
function Tgt() { this.a = 0; this.b = 0; this.c = 0; this.d = 0; }
let t = new Tgt();
for (let i = 0; i < 3000; ++i) writeAll(t, lit(i, i + 1, i + 2, i + 3));
out.push("store " + JSON.stringify(t));
const ro = new Tgt();
Object.defineProperty(ro, "c", { value: 7, writable: false });
writeAll(ro, lit(1, 2, 3, 4));
out.push("readonly " + JSON.stringify(ro));
const fr = Object.freeze(new Tgt());
writeAll(fr, lit(1, 2, 3, 4));
out.push("frozen " + JSON.stringify(fr));
const setter = new Tgt();
let sets = [];
Object.defineProperty(setter, "b", { set(v) { sets.push(v); }, get() { return -1; } });
writeAll(setter, lit(5, 6, 7, 8));
out.push("setter " + JSON.stringify(setter) + " " + sets.join(","));
t = writeAll(new Tgt(), lit(9, 8, 7, 6));
out.push("after " + JSON.stringify(t));

// Two receivers at one point, aliasing, and GC churn between calls.
s = 0;
for (let i = 0; i < 3000; ++i) {
  const q = lit(i, 2, 3, 4);
  const r = i % 3 ? lit(4, 3, 2, i) : q;
  s += copyMix(q, r);
  if (i % 500 === 0) { const junk = []; for (let k = 0; k < 2000; ++k) junk.push({ k, x: k }); s += junk.length; }
}
out.push("two " + s);
out.push("mixed " + copyMix({ w: 1, z: 2, y: 3, x: 4 }, lit(1, 2, 3, 4)));
const r2 = lit(1, 2, 3, 4); Object.defineProperty(r2, "x", { get() { return 100; } });
out.push("mixget " + copyMix(lit(1, 2, 3, 4), r2));
// Class instances whose layouts differ by a late field.
class P { constructor(late) { this.x = 1; if (late) this.k = 0; this.y = 2; this.z = 3; this.w = 4; } }
s = 0;
for (let i = 0; i < 2000; ++i) s += readAll(new P(i % 7 === 0));
out.push("class " + s);
console.log(out.join("\n"));
