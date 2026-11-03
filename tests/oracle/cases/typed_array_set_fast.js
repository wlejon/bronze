// TypedArray.prototype.set: the root-free fast path (number/undefined offset,
// typed-array source of a compatible encoding, or an all-number Array that
// fits) and every case it must hand to the general path unchanged.

function show(label, ta) {
  console.log(label, Array.prototype.join.call(ta, ","));
}

// Array sources: per-kind conversions.
{
  const src = [1.5, -1.5, 255.7, 256, -129, 1e10, NaN, Infinity, -0];
  show("f64", (() => { const t = new Float64Array(9); t.set(src); return t; })());
  show("f32", (() => { const t = new Float32Array(9); t.set(src); return t; })());
  show("i8", (() => { const t = new Int8Array(9); t.set(src); return t; })());
  show("u8", (() => { const t = new Uint8Array(9); t.set(src); return t; })());
  show("u8c", (() => { const t = new Uint8ClampedArray(9); t.set(src); return t; })());
  show("i16", (() => { const t = new Int16Array(9); t.set(src); return t; })());
  show("u16", (() => { const t = new Uint16Array(9); t.set(src); return t; })());
  show("i32", (() => { const t = new Int32Array(9); t.set(src); return t; })());
  show("u32", (() => { const t = new Uint32Array(9); t.set(src); return t; })());
  const f32 = new Float32Array(1);
  f32.set([0.1]);
  console.log("f32 round", f32[0], Object.is(new Float32Array([-0])[0], -0));
}

// Offsets: number, undefined, fractional, string, valueOf, out of range.
{
  const t = new Int32Array(6);
  t.set([1, 2], 4); show("off4", t);
  t.set([3]); show("offundef", t);
  t.set([5], undefined); show("offundef2", t);
  t.set([7], 2.9); show("off2.9", t);
  t.set([8], "1"); show("offstr", t);
  t.set([9], { valueOf() { return 3; } }); show("offvalueOf", t);
  t.set([10], NaN); show("offNaN", t);
  try { t.set([1, 2, 3], 4); } catch (e) { console.log("overflow", e.constructor.name); }
  show("after overflow", t);
  try { t.set([1], -1); } catch (e) { console.log("negative", e.constructor.name); }
  try { t.set([1], Infinity); } catch (e) { console.log("infinite", e.constructor.name); }
  try { t.set([], 7); } catch (e) { console.log("empty past end", e.constructor.name); }
  t.set([], 6); console.log("empty at end ok");
}

// Array sources the fast path must not take.
{
  const t = new Float64Array(5);
  const holes = [1, , 3];
  t.set(holes); show("holes", t);
  // An index on the chain fills the hole. (Not on Array.prototype itself,
  // which bronze refuses to decorate with an index.)
  const withIndex = Object.create(Array.prototype);
  withIndex[1] = 42;
  Object.setPrototypeOf(holes, withIndex);
  t.set(holes); show("holes proto", t);
  const objs = [{ valueOf() { return 7; } }, "8", true, null, undefined];
  t.set(objs); show("mixed", t);
  const order = [];
  const t2 = new Int8Array(3);
  t2.set([{ valueOf() { order.push("a"); return 1; } }, 2, { valueOf() { order.push("c"); return 3; } }]);
  console.log("valueOf order", order.join(""), Array.prototype.join.call(t2, ","));
  // A valueOf that throws leaves the earlier stores in place.
  const t3 = new Int8Array(3);
  try { t3.set([1, { valueOf() { throw new Error("boom"); } }, 3]); } catch (e) { console.log("throw", e.message); }
  show("partial", t3);
  // An array source whose own iterator is overridden still copies by index:
  // %TypedArray%.prototype.set reads `length` and indices, never @@iterator.
  const iterated = [1, 2];
  iterated[Symbol.iterator] = function* () { yield 99; };
  t.fill(0);
  t.set(iterated); show("ignores iterator", t);
}

// Typed-array sources: same buffer, overlapping, encoding changes.
{
  const buf = new ArrayBuffer(16);
  const u8 = new Uint8Array(buf);
  for (let i = 0; i < 16; i++) u8[i] = i;
  u8.set(u8.subarray(0, 8), 4); show("overlap fwd", u8);
  for (let i = 0; i < 16; i++) u8[i] = i;
  u8.set(u8.subarray(4, 12), 0); show("overlap back", u8);
  for (let i = 0; i < 16; i++) u8[i] = i;
  const i16 = new Int16Array(buf, 0, 4);
  u8.set(i16, 2); show("u8<-i16 same buffer", u8);
  for (let i = 0; i < 16; i++) u8[i] = i;
  const asU16 = new Uint16Array(buf);
  asU16.set(u8.subarray(0, 8)); show("u16<-u8 same buffer", asU16);
  const f32 = new Float32Array([1.5, -2.25, 3e38, NaN]);
  const f64 = new Float64Array(4);
  f64.set(f32); show("f64<-f32", f64);
  const i8 = new Int8Array(4);
  i8.set(new Float64Array([300, -300, 1.9, -1.9])); show("i8<-f64", i8);
  const u8c = new Uint8ClampedArray(3);
  u8c.set(new Int16Array([-5, 300, 128])); show("u8c<-i16", u8c);
  const big = new BigInt64Array(2);
  try { big.set(new Int32Array([1, 2])); } catch (e) { console.log("bigint<-int", e.constructor.name); }
  try { new Int32Array(2).set(new BigInt64Array(2)); } catch (e) { console.log("int<-bigint", e.constructor.name); }
  try { big.set([1, 2]); } catch (e) { console.log("bigint<-numbers", e.constructor.name); }
  big.set([3n, -4n]); show("bigint<-bigints", big);
  try { new Uint8Array(2).set(new Uint8Array(3)); } catch (e) { console.log("ta overflow", e.constructor.name); }
}

// Receivers: a non-typed-array this, a subclass, a frozen source array.
{
  try { Uint8Array.prototype.set.call([], [1]); } catch (e) { console.log("bad this", e.constructor.name); }
  class Sub extends Float32Array {}
  const s = new Sub(3);
  s.set(Object.freeze([1, 2, 3])); show("subclass", s);
}

// A warm loop over the fast path, with GC pressure between calls.
{
  const t = new Float32Array(64);
  const src = [];
  for (let i = 0; i < 64; i++) src.push(i * 0.5);
  let sum = 0;
  for (let n = 0; n < 2000; n++) {
    src[n & 63] = n;
    t.set(src);
    const junk = [{ n }, { n: n + 1 }];
    sum += t[n & 63] + junk.length;
  }
  console.log("warm", sum);
}

// One warmed call site across receivers and a patched method: the site's cached
// builtin entry must give way to a program's own function on the chain, then
// come back, and a builtin that throws or calls back into JS from that entry
// behaves as from any other call.
{
  const views = [new Float32Array(4), new Int16Array(4), new Float32Array(4)];
  function setAll(src, off) { for (const v of views) v.set(src, off); }
  for (let i = 0; i < 500; i++) setAll([i, i + 1], 1);
  show("warmed", views[1]);
  const saved = Float32Array.prototype.set;
  let calls = 0;
  Float32Array.prototype.set = function (src, off) { calls++; return saved.call(this, [-1], off); };
  setAll([7, 8], 2);
  console.log("patched", calls, Array.prototype.join.call(views[0], ","), Array.prototype.join.call(views[1], ","));
  Float32Array.prototype.set = saved;
  setAll([9, 9], 2);
  show("restored", views[0]);
  for (let i = 0; i < 3; i++) {
    try { setAll([1, 2, 3, 4, 5], 0); } catch (e) { console.log("site throws", i, e.constructor.name); }
  }
  let seen = 0;
  setAll([{ valueOf() { seen++; return 5; } }], 0);
  console.log("callback", seen, views[2][0]);
  function isArr(x) { return Array.isArray(x); }
  let n = 0;
  for (let i = 0; i < 300; i++) if (isArr(i & 1 ? [] : {})) n++;
  console.log("isArray site", n);
}
