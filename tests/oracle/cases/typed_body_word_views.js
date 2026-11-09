// A loop-free body over views it is handed (lower/typed_body_version.h) with
// an integer view among them: the copy is entered when every receiver is a
// word view (Int32Array, Uint32Array, Float32Array, Float64Array) and flags
// its element reads and writes. pixi.js's batcher shape: stores into a
// Float32Array and a Uint32Array over one buffer at `index + k`. Every case
// below goes through the same functions after the fast copy is warm, and
// must answer what node answers: ToInt32 / ToUint32 of every kind of Number
// (fractions, negatives, 2^31..2^32, 2^53, 2^63 and up, NaN (whose float
// bits are the implementation's to choose, so no NaN is negated here), the
// infinities, -0), out-of-range, fractional, negative, -0, NaN and string
// indices, values that are no Number (ToNumber runs valueOf once, before the
// index is looked at, even for an index outside the view), a BigInt, a
// detached buffer, a resizable buffer shrunk and grown under a
// length-tracking view and a fixed one, and receivers that are no word view
// (a plain Array, a Proxy, a Uint8Array, a BigInt64Array, an object).

function r(x) { return Object.is(x, -0) ? "-0" : typeof x === "number" ? String(x) : typeof x + ":" + String(x); }
function show(a) { return Array.prototype.map.call(a, r).join(","); }

// The batcher's body: one float view, one integer view, an untyped index.
function pack(el, f32, u32, index, tex) {
  const t = el.t;
  const argb = el.color;
  const packed = tex << 16 | el.round & 65535;
  f32[index + 0] = t.a * el.x + t.tx;
  f32[index + 1] = t.d * el.y + t.ty;
  f32[index + 2] = el.u;
  u32[index + 3] = argb;
  u32[index + 4] = packed;
  f32[index + 5] = el.v;
}

// Reads through the same kinds of views, summed and listed.
function readBack(a, b, index) {
  const x = a[index + 0], y = a[index + 1], z = b[index + 3], w = b[index + 4];
  return [r(x), r(y), r(z), r(w), r(a[index + 5])].join(" ");
}

const el = { t: { a: 1.5, d: 2, tx: 3, ty: -4 }, x: 1, y: 2, u: 0.1, v: 0.7, color: 0xff00ff00, round: 1 };
const buf = new ArrayBuffer(4 * 6 * 64);
const F = new Float32Array(buf), U = new Uint32Array(buf), I = new Int32Array(buf), D = new Float64Array(buf);
let h = 0;
for (let r2 = 0; r2 < 300; r2++) {
  for (let i = 0; i < 64; i++) {
    el.x = i; el.y = r2;
    pack(el, F, U, i * 6, i & 7);
  }
}
for (let i = 0; i < U.length; i++) h = (Math.imul(h, 31) + U[i]) >>> 0;
console.log("warm", h, readBack(F, U, 6), readBack(F, I, 12));
for (let i = 0; i < 300; i++) readBack(D, I, 0);
console.log("f64/i32", readBack(D, I, 0), readBack(I, U, 0));

// ToUint32 / ToInt32 of every kind of Number, through the integer stores.
const small = new ArrayBuffer(4 * 8);
const sf = new Float32Array(small), su = new Uint32Array(small), si = new Int32Array(small);
const colors = [0, -1, 1.9, -1.9, 2147483647, 2147483648, 4294967295, 4294967296, 4294967297, -2147483649,
  2 ** 53, 2 ** 53 + 2, 2 ** 63, -(2 ** 63), 2 ** 64 + 4096, 1e21, -1e300, NaN, Infinity, -Infinity, -0,
  5e-324, 0.5, 3000000000.7];
for (const c of colors) {
  pack({ t: el.t, x: 1, y: 1, u: c, v: c === c ? -c : 1, color: c, round: 0 }, sf, su, 0, 1);
  const a = show(su);
  pack({ t: el.t, x: 1, y: 1, u: c, v: c === c ? -c : 1, color: c, round: 0 }, sf, si, 0, 1);
  console.log("conv", r(c), a, show(si), show(sf));
}

// Indices that are no valid integer index: dropped stores, undefined reads.
const k = new ArrayBuffer(4 * 8);
const kf = new Float32Array(k), ku = new Uint32Array(k);
for (const idx of [-1, -0, 0.5, 2.5, 3, 4, 100, -6, NaN, Infinity, 2 ** 32, "1", "x", "-0"]) {
  kf.fill(0);
  try {
    pack(el, kf, ku, idx, 2);
    console.log("idx", r(idx), show(ku), show(kf), readBack(kf, ku, idx));
  } catch (e) {
    console.log("idx", r(idx), "threw", e instanceof TypeError);
  }
}

// Values that are no Number: valueOf runs once a store, in order, before
// the index is looked at — also at an index outside the view.
const log = [];
const v = (name, n) => ({ valueOf() { log.push(name); return n; } });
const vel = { t: { a: 1, d: 1, tx: 0, ty: 0 }, x: 1, y: 1, u: v("u", 7.25), v: v("v", -1), color: v("color", -2), round: 0 };
pack(vel, kf, ku, 0, 0);
pack(vel, kf, ku, 7, 0);
console.log("valueOf", log.join(","), show(ku), show(kf));
for (const bad of [10n, Symbol("s")]) {
  try {
    pack({ t: el.t, x: 1, y: 1, u: 1, v: 1, color: bad, round: 0 }, kf, ku, 0, 0);
    console.log("bad stored");
  } catch (e) {
    console.log("bad", typeof bad, e instanceof TypeError, show(ku));
  }
}
console.log("other values", (() => {
  pack({ t: el.t, x: 1, y: 1, u: "2.5", v: null, color: undefined, round: true }, kf, ku, 0, 3);
  return show(ku) + " " + show(kf);
})());

// A detached buffer: every store dropped, every read undefined, and still
// ToNumber first.
const dbuf = new ArrayBuffer(4 * 8);
const df = new Float32Array(dbuf), du = new Uint32Array(dbuf);
pack(el, df, du, 0, 1);
const kept = dbuf.transfer();
log.length = 0;
pack(vel, df, du, 0, 1);
console.log("detached", df.length, du.length, readBack(df, du, 0), log.join(","), new Uint32Array(kept)[3]);

// A resizable buffer: a length-tracking view follows the buffer, a fixed
// one goes out of bounds when the buffer shrinks under it and comes back.
const rb = new ArrayBuffer(4 * 8, { maxByteLength: 4 * 16 });
const tf = new Float32Array(rb), tu = new Uint32Array(rb), fixed = new Uint32Array(rb, 0, 8);
pack(el, tf, fixed, 0, 1);
console.log("rs", tf.length, show(tu));
rb.resize(4 * 4);
pack(el, tf, tu, 0, 2);
pack(el, tf, fixed, 0, 3);
console.log("rs shrunk", tf.length, fixed.length, show(tu), readBack(tf, fixed, 0), readBack(tf, tu, 0));
rb.resize(4 * 16);
pack(el, tf, tu, 10, 4);
pack(el, tf, fixed, 2, 5);
console.log("rs grown", tf.length, fixed.length, show(tu), readBack(tf, tu, 10));

// Receivers that are no word view take the original body.
const arr = [];
pack(el, arr, arr, 1, 1);
console.log("array", arr.length, show(arr));
const seen = [];
const ptarget = new Uint32Array(8);
const px = new Proxy(ptarget, { set(t, key, val) { seen.push(key + "=" + r(val)); t[key] = val; return true; } });
pack(el, kf, px, 0, 1);
console.log("proxy", seen.join(" "), show(ptarget));
const u8 = new Uint8Array(8), c8 = new Uint8ClampedArray(8), i16 = new Int16Array(8);
pack(el, kf, u8, 0, 1);
pack(el, kf, c8, 0, 1);
pack(el, kf, i16, 0, 1);
console.log("narrow", show(u8), show(c8), show(i16));
try { pack(el, kf, new BigInt64Array(8), 0, 1); } catch (e) { console.log("bigint view", e instanceof TypeError); }
const obj = {};
pack(el, obj, obj, 2, 1);
console.log("object", JSON.stringify(obj));
// And the fast copy again after all of that.
pack(el, F, U, 6, 6);
console.log("again", readBack(F, U, 6));
