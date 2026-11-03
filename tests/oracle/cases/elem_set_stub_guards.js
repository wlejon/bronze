// The module's keyed-write stub (codegen-brass il_elem_stub.cpp) stores a
// computed write `o[k] = v` on a plain receiver straight into the slot the
// computed-store cache names. Each section warms a (shape, key) pair until the
// stub hits, then changes one thing the hit stands on and writes again.

function put(o, k, v) { o[k] = v; }
function putStrict(o, k, v) { 'use strict'; o[k] = v; }
function warm(o, k, n) { for (let i = 0; i < n; i++) put(o, k, i); return o[k]; }

// --- 1. own data, inline and overflow slots ---------------------------------
const wide = {};
for (let i = 0; i < 40; i++) wide['p' + i] = 0;
for (let r = 0; r < 50; r++) for (let i = 0; i < 40; i++) put(wide, 'p' + i, r * i);
let s = 0;
for (let i = 0; i < 40; i++) s += wide['p' + i];
console.log(s, warm(wide, 'p39', 30));

// --- 2. heap values written through the stub survive collections -------------
const holder = { a: null, b: null };
for (let i = 0; i < 2000; i++) { put(holder, (i & 1) ? 'a' : 'b', { n: i, s: 'v' + i }); }
const junk = [];
for (let i = 0; i < 2000; i++) junk.push({ i, t: 'x' + i });
console.log(holder.a.n, holder.a.s, holder.b.n, holder.b.s, junk.length);

// --- 3. freeze, seal, preventExtensions and non-writable after warming --------
const fz = { k: 0 };
warm(fz, 'k', 50);
Object.freeze(fz);
put(fz, 'k', 'changed');
let threw = 'no';
try { putStrict(fz, 'k', 'changed'); } catch (e) { threw = e instanceof TypeError; }
console.log(fz.k, threw);
const nw = { k: 0, other: 1 };
warm(nw, 'k', 50);
Object.defineProperty(nw, 'k', { writable: false });
put(nw, 'k', 'changed');
console.log(nw.k);
const sealed = { k: 0 };
warm(sealed, 'k', 50);
Object.seal(sealed);
put(sealed, 'k', 'sealed write');
put(sealed, 'fresh', 1);
console.log(sealed.k, sealed.fresh);

// --- 4. an accessor installed over a hot data key -----------------------------
const acc = { v: 0 };
warm(acc, 'v', 50);
let seen = [];
Object.defineProperty(acc, 'v', { set(x) { seen.push(x); }, get() { return 'g'; }, configurable: true });
put(acc, 'v', 'through setter');
console.log(acc.v, seen.join(','));

// --- 5. a setter on the prototype for a key the receiver lacks ----------------
const proto = {};
const child = Object.create(proto);
child.mine = 0;
warm(child, 'mine', 50);
let protoSet = 0;
Object.defineProperty(proto, 'later', { set(x) { protoSet += x; }, configurable: true });
for (let i = 0; i < 20; i++) put(child, 'later', 1);
console.log(protoSet, Object.prototype.hasOwnProperty.call(child, 'later'));

// --- 6. adds (transitions) and deletes ----------------------------------------
const grow = {};
for (let i = 0; i < 30; i++) put(grow, 'g' + i, i);
console.log(Object.keys(grow).length, grow.g29);
const d = { x: 1, y: 2 };
warm(d, 'y', 50);
delete d.y;
put(d, 'y', 'readded');
console.log(Object.keys(d).join(','), d.y);

// --- 7. number keys and number values in f64-shaped slots ---------------------
const caps = {};
for (const id of [3042, 2884, 2929]) caps[id] = false;
for (let r = 0; r < 100; r++) for (const id of [3042, 2884, 2929]) { put(caps, id, true); put(caps, id, false); }
console.log(JSON.stringify(caps));
const nums = { x: 0.5, y: 1.5 };
for (let i = 0; i < 100; i++) put(nums, (i & 1) ? 'x' : 'y', i + 0.25);
put(nums, 'x', 'now a string');
put(nums, 'y', { o: 1 });
console.log(nums.x, JSON.stringify(nums.y));

// --- 8. a fresh, content-equal key string ------------------------------------
function cat(a, b) { return a + b; }
const ks = { alpha: 0 };
for (let i = 0; i < 100; i++) put(ks, (i & 1) ? 'alpha' : cat('alp', 'ha'), i);
console.log(ks.alpha);

// --- 9. receivers that are not plain objects ---------------------------------
const arr = [1, 2, 3];
for (let i = 0; i < 20; i++) { put(arr, 1, i); put(arr, 'extra', i); }
const ta = new Float64Array(2);
for (let i = 0; i < 20; i++) put(ta, 0, i + 0.5);
const fn = function () {};
for (let i = 0; i < 20; i++) put(fn, 'stat', i);
console.log(arr[1], arr.extra, ta[0], fn.stat);
