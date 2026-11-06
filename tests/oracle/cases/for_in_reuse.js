// for-in records are reused once a loop is exhausted: nested, recursive,
// broken-out, thrown-out and generator-suspended loops over one shape must each
// keep their own position.
function keysOf(o) { const out = []; for (const k in o) out.push(k); return out.join(","); }
const a = { x: 1, y: 2, z: 3 };
const b = { x: 4, y: 5, z: 6 };
// Nested over the same shape: the inner loop exhausts (and frees) a record
// on every outer step while the outer one is mid-walk.
const pairs = [];
for (const i in a) for (const j in b) pairs.push(i + j);
console.log(pairs.join(" "));
// Recursion deeper than the pool.
function deep(n) { const r = []; for (const k in a) { r.push(k + n); if (n > 0 && k === "y") r.push("[" + deep(n - 1) + "]"); } return r.join(","); }
console.log(deep(6));
// break abandons a record; the next loops must not see its cursor.
for (const k in a) { if (k === "y") break; }
console.log(keysOf(a), keysOf(b));
// A throw out of the body.
try { for (const k in a) { if (k === "x") throw new Error("out " + k); } } catch (e) { console.log(e.message); }
console.log(keysOf(a));
// A generator suspended inside a for-in while other for-ins run and finish.
function* gen(o) { for (const k in o) yield k; }
const g1 = gen(a), g2 = gen(b);
const seq = [];
seq.push(g1.next().value);
seq.push(keysOf(b));
seq.push(g2.next().value, g1.next().value, g2.next().value);
seq.push(keysOf(a));
seq.push(g1.next().value, g1.next().done, g2.next().value, g2.next().done);
console.log(seq.join(" "));
// for-of and destructuring over arrays still behave after for-ins have run.
const arr = ["p", "q"];
for (const k in a) { const [first, second, third] = arr; console.log(k, first, second, String(third)); }
const it = arr[Symbol.iterator]();
for (const k in b) {}
console.log(it.next().value, it.next().value, it.next().done);
// Arrays, strings and empty objects through the same path.
console.log(keysOf([7, 8, 9]), keysOf("ab"), keysOf({}), keysOf(null));
let count = 0;
for (let r = 0; r < 1000; r++) for (const k in a) count++;
console.log(count);
