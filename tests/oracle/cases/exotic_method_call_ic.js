// Exotic-receiver method-IC smoke: every latchable class, plus every
// shadowing story the guard must catch.

// --- Array: hot monomorphic sites latch and hit ---
let arr = [];
for (let i = 0; i < 2000; i++) arr.push(i);
let sum = 0;
for (let i = 0; i < 2000; i++) sum += arr.shift();
console.log("array push/shift", arr.length, sum);

// pop, indexOf, includes, join through one hot loop each
let a2 = [];
for (let i = 0; i < 200; i++) a2.push(i * 2);
let found = 0;
for (let i = 0; i < 200; i++) if (a2.includes(i)) found++;
console.log("includes", found, a2.indexOf(88), a2.join("-").length);

// --- Array shadowing: an own property must win AFTER the site latched ---
function callPush(a, v) { return a.push(v); }
let plain = [];
callPush(plain, 1); callPush(plain, 2); callPush(plain, 3);  // latch + hits
let shadowed = [];
shadowed.push = function (v) { return "shadow:" + v; };
console.log("own-prop shadow", callPush(shadowed, 9), callPush(plain, 4), plain.length);

// --- Array subclass: overridden method must win at the same site ---
class LoudArray extends Array {
  push(v) { return "loud:" + v; }
}
let loud = new LoudArray();
console.log("subclass shadow", callPush(loud, 7), callPush(plain, 5), plain.length);

// A subclass that does NOT override still gets the builtin (box present ->
// helper path every call; answer must not change)
class PlainSub extends Array {}
let ps = new PlainSub();
for (let i = 0; i < 50; i++) callPush(ps, i);
console.log("subclass no-override", ps.length, ps instanceof Array);

// An array that gains a named property (box) AFTER latching keeps working
let boxy = [];
for (let i = 0; i < 50; i++) callPush(boxy, i);
boxy.note = "hello";
for (let i = 0; i < 50; i++) callPush(boxy, i);
console.log("late box", boxy.length, boxy.note);

// --- Map ---
function mget(m, k) { return m.get(k); }
function mset(m, k, v) { return m.set(k, v); }
let m = new Map();
for (let i = 0; i < 1000; i++) mset(m, i, i * 3);
let msum = 0;
for (let i = 0; i < 1000; i++) msum += mget(m, i);
console.log("map get/set", m.size, msum);
console.log("map has/delete", m.has(500), m.delete(500), m.has(500), m.size);

// Map shadowing after latch
let m2 = new Map();
mset(m2, "k", "v");
m2.get = function (k) { return "shadowGet:" + k; };
console.log("map shadow", mget(m2, "k"), mget(m, 1));

// Map subclass override
class MyMap extends Map {
  get(k) { return "sub:" + k; }
}
let mm = new MyMap();
mm.set("x", "y");
console.log("map subclass", mget(mm, "x"), mget(m, 2));

// --- Set ---
let s = new Set();
for (let i = 0; i < 500; i++) s.add(i % 100);
let scount = 0;
for (let i = 0; i < 200; i++) if (s.has(i)) scount++;
console.log("set add/has", s.size, scount);

// --- WeakMap (the three.js WebGLProperties pattern) ---
let wm = new WeakMap();
let keys = [];
for (let i = 0; i < 300; i++) keys.push({ id: i });
function wget(w, k) { return w.get(k); }
function wset(w, k, v) { return w.set(k, v); }
for (let i = 0; i < 300; i++) wset(wm, keys[i], { v: i });
let wsum = 0;
for (let i = 0; i < 300; i++) wsum += wget(wm, keys[i]).v;
console.log("weakmap", wsum, wm.has(keys[0]), wm.has({}));

// --- WeakSet ---
let ws = new WeakSet();
for (let i = 0; i < 100; i++) ws.add(keys[i]);
console.log("weakset", ws.has(keys[5]), ws.has(keys[200]));

// --- forEach with a callback (env-carrying callee as ARGUMENT, receiver exotic) ---
let feSum = 0;
let base = 100;
m.clear();
for (let i = 0; i < 10; i++) m.set(i, i);
m.forEach(function (v, k) { feSum += v + k + base; });
arr = [1, 2, 3];
arr.forEach(function (v) { feSum += v + base; });
console.log("forEach", feSum);

// --- mixed receiver kinds at ONE site (kind guard must discriminate) ---
function anyPush(r, v) { return r.add ? r.add(v) : r.push(v); }
let mixArr = [];
let mixSet = new Set();
for (let i = 0; i < 100; i++) { anyPush(mixArr, i); anyPush(mixSet, i); }
console.log("mixed", mixArr.length, mixSet.size);

// --- a site that sees a Plain object AND an Array (form ping-pong) ---
function callJoin(o) { return o.join(","); }
let plainObj = { join: (sep) => "plain" + sep };
let joined = "";
for (let i = 0; i < 20; i++) {
  joined = callJoin([i, i + 1]);
  joined += callJoin(plainObj);
}
console.log("plain/array mix", joined);

// --- spread method call on exotic receiver ---
let spreadArr = [];
let parts = [7, 8, 9];
spreadArr.push(...parts);
console.log("spread", spreadArr.length, spreadArr[2]);

console.log("done");

// --- TypedArray: hot .set/.fill/.indexOf latch and hit; no shadowing channel exists ---
let f32 = new Float32Array(16);
let src = new Float32Array(16);
for (let i = 0; i < 16; i++) src[i] = i * 0.5;
function upload(dst, s) { dst.set(s); }
let acc = 0;
for (let i = 0; i < 2000; i++) { upload(f32, src); acc += f32[3]; }
console.log("ta set", acc, f32[15]);

f32.fill(2.5, 0, 4);
console.log("ta fill/indexOf", f32[0], f32.indexOf(2.5), f32.indexOf(999));

// subarray/slice through a hot site
function sub(v) { return v.subarray(2, 6); }
let subLen = 0;
for (let i = 0; i < 500; i++) subLen = sub(f32).length;
console.log("ta subarray", subLen, sub(src)[0]);

// per-view constructor must stay per-view (the memo must never serve it)
let f64 = new Float64Array(4);
let u8 = new Uint8Array(4);
console.log("ta ctors", f32.constructor === Float32Array, f64.constructor === Float64Array,
            u8.constructor === Uint8Array, f32.constructor === f64.constructor);

// mixed TypedArray kinds at ONE site (one shared method table -> same native)
function fillIt(v) { v.fill(1); return v[0]; }
console.log("ta mixed kinds", fillIt(new Float64Array(2)), fillIt(new Uint8Array(2)),
            fillIt(new Float32Array(2)));

// a site mixing a typed array and a PLAIN receiver
function callSet(o, a, b) { return o.set(a, b); }
let plainSet = { set: (a, b) => "plain:" + a + ":" + b };
let mix32 = new Float32Array(8);
let mixResult = "";
for (let i = 0; i < 30; i++) {
  callSet(mix32, [i, i + 1], 2);
  mixResult = callSet(plainSet, i, i + 1);
}
console.log("ta plain mix", mixResult, mix32[2], mix32[3]);

// Map/Set at the same site as a TypedArray (kind guard between exotic kinds)
function callHasOrIndex(r, k) { return r.has ? r.has(k) : r.indexOf(k) >= 0; }
let hs = new Set([1, 2, 3]);
let hi = new Float32Array([1, 2, 3]);
let agree = true;
for (let i = 0; i < 50; i++) {
  if (callHasOrIndex(hs, 2) !== callHasOrIndex(hi, 2)) agree = false;
}
console.log("ta/set mixed", agree);

console.log("done2");
