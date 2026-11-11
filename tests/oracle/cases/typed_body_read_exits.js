// A typed body with named reads between its stores (pixi's batcher: `uvs.x0`
// between the writes of a quad). In the body's typed copy each read's guard
// is an exit to the original body, so every way a read can miss has to land
// in the original at that read with the stores before it done and the ones
// after it not: another shape, a getter that looks at the view, a value that
// is no Number, a receiver that throws.
function pack(el, f32, u32, index, id) {
  const t = el.t;
  const a = t.a, b = t.b, tx = t.tx;
  const uvs = el.uvs;
  const argb = el.color;
  f32[index + 0] = a * 2 + tx;
  f32[index + 1] = b * 3 + tx;
  f32[index + 2] = uvs.x0;
  f32[index + 3] = uvs.y0;
  u32[index + 4] = argb;
  u32[index + 5] = id << 16 | el.round & 65535;
  f32[index + 6] = a * 5 + tx;
  f32[index + 7] = b * 7 + tx;
  f32[index + 8] = uvs.x1;
  f32[index + 9] = uvs.y1;
  u32[index + 10] = argb;
  u32[index + 11] = id;
}

const buf = new ArrayBuffer(12 * 4 * 8);
const f32 = new Float32Array(buf);
const u32 = new Uint32Array(buf);
function sum() {
  let s = 0;
  for (let i = 0; i < u32.length; i++) s = (s * 31 + u32[i]) >>> 0;
  return s;
}
function el(uvs) {
  return { t: { a: 1.5, b: -2.25, tx: 10 }, uvs, color: 0xff8040, round: 1 };
}

// The plain case, hot.
const plain = el({ x0: 0.1, y0: 0.2, x1: 0.3, y1: 0.4 });
for (let n = 0; n < 3000; n++) pack(plain, f32, u32, (n & 7) * 12, n & 255);
console.log("plain", sum());

// Other shapes of the read's receiver: keys in another order, an extra key
// first, a prototype holding one of them.
u32.fill(0);
pack(el({ y1: 4, x1: 3, y0: 2, x0: 1 }), f32, u32, 0, 1);
pack(el({ z: 0, x0: 5, y0: 6, x1: 7, y1: 8 }), f32, u32, 12, 2);
pack(el(Object.assign(Object.create({ x1: 11 }), { x0: 9, y0: 10, y1: 12 })), f32, u32, 24, 3);
console.log("shapes", sum(), Array.from(f32.subarray(0, 4)).join(), f32[12 + 8], f32[24 + 8]);

// A getter sees the stores before its read and none after it.
u32.fill(0);
const seen = [];
pack(el({
  x0: 1,
  get y0() { seen.push(f32[2], f32[3], f32[6]); return 2; },
  x1: 3,
  get y1() { seen.push(f32[8], f32[9], u32[10]); return 4; },
}), f32, u32, 0, 4);
console.log("getter", sum(), seen.join());

// Values that are no Number: a string, undefined, an object whose valueOf
// looks at the view.
u32.fill(0);
pack(el({ x0: "1.5", y0: undefined, x1: null, y1: { valueOf() { return f32[8] + u32[5]; } } }), f32, u32, 0, 5);
console.log("values", sum(), f32[2], f32[3], f32[8], f32[9]);

// A read that throws leaves the earlier stores and makes none of the later.
u32.fill(0);
const bad = el(null);
try {
  pack(bad, f32, u32, 0, 6);
} catch (e) {
  console.log("throws", e instanceof TypeError, sum(), f32[0], f32[1], f32[2]);
}
u32.fill(0);
try {
  pack(el({ x0: 1, y0: 2, get x1() { throw new RangeError("x1"); }, y1: 4 }), f32, u32, 0, 7);
} catch (e) {
  console.log("throws", e.message, sum(), f32[7], f32[8], u32[10]);
}

// And the plain case again after all of them.
u32.fill(0);
for (let n = 0; n < 3000; n++) pack(plain, f32, u32, (n & 7) * 12, n & 255);
console.log("plain", sum());

// A view detached by a getter mid-way: the stores after it are dropped.
const gone = new ArrayBuffer(96);
const g32 = new Float32Array(gone), gu = new Uint32Array(gone);
pack(el({ x0: 1, get y0() { gone.transfer(); return 2; }, x1: 3, y1: 4 }), g32, gu, 0, 8);
console.log("detached", g32.length, gu.length);
