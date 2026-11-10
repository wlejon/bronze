// a[a.length] = v appends inline (branch_on_array_store) and in bronze_elem_set
// when the hazard is clear, the array has no properties object and the slot is
// inside the elements block. Every way out of that rule must still give the
// spec's answer.

function fill(a, n) {
  for (let i = 0; i < n; i++) a[i] = i * 2;
  return a;
}
function appendAny(a, v) { a[a.length] = v; return a.length; }

// Plain growth past capacity, mixed kinds (NUMBERS bit cleared by non-numbers).
const a = fill([], 50);
console.log(a.length, a[0], a[49], a.reduce((s, x) => s + x, 0));
appendAny(a, "s"); appendAny(a, undefined); appendAny(a, null); appendAny(a, { k: 1 });
console.log(a.length, typeof a[50], a[51], a[52], a[53].k, a.indexOf(undefined));
a[a.length] = 1.5;
console.log(a.length, a[a.length - 1], a.includes(1.5));

// Pre-sized and shrunk arrays: append into a block with spare capacity.
const b = new Array(10); b.length = 0;
for (let i = 0; i < 20; i++) b[i] = i;
console.log(b.length, b.join(","));
b.length = 3; b[3] = "x"; b[b.length] = "y";
console.log(b.length, b.join(","));

// Append after shift (head_offset non-zero).
const c = fill([], 8); c.shift(); c.shift();
c[c.length] = 100; c[c.length] = 101;
console.log(c.length, c.join(","));

// Not an append: index past length leaves holes.
const d = [1, 2]; d[5] = 6;
console.log(d.length, 3 in d, d[5], JSON.stringify(d));

// Frozen / sealed / non-extensible arrays refuse the append.
for (const [name, op] of [["freeze", Object.freeze], ["seal", Object.seal], ["preventExtensions", Object.preventExtensions]]) {
  const f = op([1, 2, 3]);
  f[3] = 4; f[f.length] = 5;
  console.log(name, f.length, f[3], Object.isExtensible(f));
  try { (function () { "use strict"; f[f.length] = 9; })(); console.log("no throw"); }
  catch (e) { console.log(name, "strict", e.constructor.name); }
}

// Array with a named property (properties object) still appends correctly.
const e = [1, 2]; e.tag = "t"; e[2] = 3; e[e.length] = 4;
console.log(e.length, e.join(","), e.tag, Object.keys(e).join(","));

// (bronze refuses index properties on Array.prototype and a non-writable
// array length outright, so those two hazards are tested through
// Object.prototype and a proxy prototype instead.)
let log = [];

// Index setter on Object.prototype.
Object.defineProperty(Object.prototype, "2", {
  configurable: true,
  set(v) { log.push("obj proto " + v); },
});
const h = [0, 1]; h[2] = "x";
console.log(h.length, Object.hasOwn(h, 2), log.join("|"));
delete Object.prototype[2];
const h2 = [0, 1]; h2[2] = "x";
console.log(h2.length, h2[2]);

// Array subclass and an array whose prototype is a proxy.
class MyArr extends Array {}
const m = new MyArr(); for (let i = 0; i < 5; i++) m[m.length] = i;
console.log(m.length, m instanceof MyArr, m.join(","));
const pa = [1];
Object.setPrototypeOf(pa, new Proxy(Array.prototype, {
  set(t, k, v, r) { log.push("proxy set " + String(k)); return Reflect.set(t, k, v, r); },
}));
log = []; pa[1] = 2; pa[pa.length] = 3;
console.log(pa.length, pa[1], pa[2], log.join("|"));

// Length changes inside the loop.
const lc = [];
for (let i = 0; i < 10; i++) { lc[lc.length] = i; if (i % 3 === 2) lc.length -= 1; }
console.log(lc.length, lc.join(","));

// Numeric-string and symbol keys.
const ns = [1]; ns["1"] = 2; ns["01"] = 3; ns[Symbol.for("s")] = 4;
console.log(ns.length, ns[1], ns["01"], ns[Symbol.for("s")]);
