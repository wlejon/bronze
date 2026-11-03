// The module's keyed-read stub (codegen-brass il_elem_stub.cpp) answers a
// computed read on a plain receiver from the computed-read cache without
// calling the runtime. Each section warms a (shape, key) pair until the stub
// hits, then changes one thing the hit stands on and reads again.

function get(o, k) { return o[k]; }
function warm(o, k, n) { let r; for (let i = 0; i < n; i++) r = get(o, k); return r; }

// --- 1. own data, inline and overflow slots ---------------------------------
const wide = {};
for (let i = 0; i < 40; i++) wide['p' + i] = i * 3;
let s = 0;
for (let r = 0; r < 50; r++) for (let i = 0; i < 40; i++) s += get(wide, 'p' + i);
console.log(s);
console.log(warm(wide, 'p0', 100), warm(wide, 'p39', 100));

// --- 2. a write to the slot is seen; a shape change re-proves ---------------
const o = { a: 1, b: 2 };
console.log(warm(o, 'b', 100));
o.b = 7;
console.log(get(o, 'b'));
o.c = 3;
console.log(warm(o, 'b', 100), get(o, 'c'));

// --- 3. delete, then re-add ---------------------------------------------------
const d = { x: 1, y: 2, z: 3 };
console.log(warm(d, 'y', 100));
delete d.y;
console.log(get(d, 'y'), warm(d, 'z', 50));
d.y = 9;
console.log(warm(d, 'y', 50));

// --- 4. an accessor installed over a hot data key -----------------------------
const acc = { v: 1 };
console.log(warm(acc, 'v', 100));
let calls = 0;
Object.defineProperty(acc, 'v', { get() { calls++; return 42; }, configurable: true });
console.log(warm(acc, 'v', 10), calls);

// --- 5. absent, then present on the prototype ---------------------------------
const proto = {};
const child = Object.create(proto);
child.own = 1;
console.log(warm(child, 'later', 100));
proto.later = 'from proto';
console.log(get(child, 'later'));
Object.defineProperty(proto, 'later', { get() { return 'getter'; } , configurable: true});
console.log(get(child, 'later'));
delete proto.later;
console.log(get(child, 'later'));
Object.setPrototypeOf(child, { later: 'new proto' });
console.log(get(child, 'later'));

// --- 6. absent, then own ------------------------------------------------------
const late = { a: 1 };
console.log(warm(late, 'b', 100));
late.b = 5;
console.log(get(late, 'b'));

// --- 7. number keys: integral, -0, fractional, NaN, huge ----------------------
const caps = {};
const ids = [3042, 2884, 2929, 2960, 32823];
for (const id of ids) caps[id] = false;
let flips = 0;
for (let r = 0; r < 100; r++) {
  for (const id of ids) {
    if (caps[id] !== true) { caps[id] = true; flips++; }
    if (caps[id] !== false) { caps[id] = false; flips++; }
  }
}
console.log(flips, caps[2884], caps[1]);
const nk = { 0: 'zero', 1.5: 'one and a half', NaN: 'nan', 1e21: 'big', '-0': 'minus zero string' };
for (let r = 0; r < 50; r++) { get(nk, 0); get(nk, -0); get(nk, 1.5); get(nk, NaN); get(nk, 1e21); }
console.log(get(nk, 0), get(nk, -0), get(nk, 1.5), get(nk, NaN), get(nk, 1e21), get(nk, '-0'));

// --- 8. a number key and its string spelling share one property ---------------
const both = { 7: 'seven' };
console.log(warm(both, 7, 50), warm(both, '7', 50));
both['7'] = 'SEVEN';
console.log(get(both, 7));

// --- 9. boolean, null and undefined keys --------------------------------------
const odd = { true: 't', false: 'f', null: 'n', undefined: 'u' };
let bs = '';
for (let r = 0; r < 20; r++) bs = get(odd, true) + get(odd, false) + get(odd, null) + get(odd, undefined);
console.log(bs);

// --- 10. frozen, sealed and dictionary receivers ------------------------------
const fz = Object.freeze({ k: 'frozen' });
console.log(warm(fz, 'k', 50));
const dict = {};
for (let i = 0; i < 300; i++) dict['k' + i] = i;
for (let i = 0; i < 300; i += 2) delete dict['k' + i];
let ds = 0;
for (let r = 0; r < 20; r++) for (let i = 0; i < 300; i++) { const v = get(dict, 'k' + i); if (v !== undefined) ds += v; }
console.log(ds);

// --- 11. receivers that are not plain objects --------------------------------
const arr = [10, 20, 30];
arr.extra = 'e';
const str = new String('abc');
const fn = function () {};
fn.prop = 'fp';
const m = new Map();
console.log(warm(arr, 1, 20), warm(arr, 'extra', 20), warm(arr, 'length', 20));
console.log(warm(str, 0, 20), warm(str, 'length', 20), warm('xyz', 2, 20), warm(fn, 'prop', 20));
console.log(typeof warm(m, 'get', 20), warm(Math, 'PI', 20) === Math.PI);

// --- 12. symbol and object keys -----------------------------------------------
const sym = Symbol('s');
const sk = { [sym]: 'symbol value', '[object Object]': 'obj key' };
console.log(warm(sk, sym, 20), warm(sk, {}, 20));
let tos = 0;
const keyObj = { toString() { tos++; return 'a'; } };
console.log(warm({ a: 'via toString' }, keyObj, 5), tos);

// --- 13. many shapes, one key: entries that collide still answer right --------
const shapes = [];
for (let i = 0; i < 64; i++) { const x = {}; x['f' + (i % 7)] = i; x.name = 'n' + i; shapes.push(x); }
let names = '';
for (let r = 0; r < 10; r++) for (let i = 0; i < 64; i++) names = get(shapes[i], 'name');
let fsum = 0;
for (let i = 0; i < 64; i++) { const v = get(shapes[i], 'f' + (i % 7)); fsum += v; }
console.log(names, fsum);

// --- 14. class instances and prototype methods --------------------------------
class P { constructor() { this.x = 1; } m() { return 'method'; } }
class Q extends P { constructor() { super(); this.y = 2; } }
const q = new Q();
console.log(warm(q, 'x', 30), warm(q, 'y', 30), warm(q, 'm', 30)(), typeof warm(q, 'constructor', 30));
P.prototype.m = function () { return 'patched'; };
console.log(get(q, 'm')());
