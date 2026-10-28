// `.length` at one site warmed on typed arrays and read inline from the view
// header, then fed every receiver whose `length` is NOT that header: an own
// `length`, a subclass, a swapped prototype, a redefined %TypedArray% getter
// (and its restoration), detached and resized buffers, other kinds, arrays,
// strings and plain objects through the same site.
function len(x) { return x.length; }
function sumLen(xs) { let s = 0; for (let i = 0; i < xs.length; i++) s += len(xs[i]); return s; }
const f32 = new Float32Array(16), u8 = new Uint8Array(5), i16 = new Int16Array(3), f64 = new Float64Array(7);
let warm = 0;
for (let i = 0; i < 50000; i++) warm += len(f32) + len(u8);
console.log("warm", warm);
console.log("kinds", [new Int8Array(1), new Uint8ClampedArray(2), i16, new Uint16Array(4), new Int32Array(5),
  new Uint32Array(6), f64, new BigInt64Array(8), new BigUint64Array(9)].map(len).join(","));
console.log("mixed", sumLen([f32, [1, 2, 3], "abcd", { length: 11 }, u8, { length: "x" }, function (a, b) {}]));
// Own property on one view (its shape changes); its siblings are untouched.
const own = new Float32Array(4);
Object.defineProperty(own, "length", { value: 99 });
console.log("own", len(own), len(new Float32Array(4)));
// A subclass with its own getter, and one without.
class Long extends Float32Array { get length() { return 1000 + super.length; } }
class Plain extends Uint8Array {}
console.log("subclass", len(new Long(3)), len(new Plain(6)));
// A view whose prototype is swapped to one with a data `length`.
const swapped = new Int16Array(10);
Object.setPrototypeOf(swapped, { length: -5 });
console.log("swapped", len(swapped), len(i16));
// Detach by transfer: length reads 0 after.
const buf = new ArrayBuffer(64), view = new Float64Array(buf);
console.log("before transfer", len(view));
const moved = buf.transfer(16);
console.log("after transfer", len(view), len(new Uint8Array(moved)));
// Length-tracking and fixed views over a resizable buffer.
const rb = new ArrayBuffer(16, { maxByteLength: 64 });
const tracking = new Uint8Array(rb), fixed = new Uint8Array(rb, 4, 8), tail = new Uint32Array(rb, 8);
const seen = [];
for (const n of [16, 64, 10, 4, 0, 32]) { rb.resize(n); seen.push(`${n}:${len(tracking)}/${len(fixed)}/${len(tail)}`); }
console.log("resizable", seen.join(" "));
// Redefine the shared getter, read through the warmed site, then restore it.
const TA = Object.getPrototypeOf(Float32Array.prototype);
const orig = Object.getOwnPropertyDescriptor(TA, "length");
Object.defineProperty(TA, "length", { get() { return "patched"; }, configurable: true });
// (Recorded, then printed once restored: node writes stdout through
// Uint8Arrays, whose `length` these patches change.)
const patched = ["patched", len(f32), len(u8), sumLen([f32, [1]])];
Object.defineProperty(TA, "length", orig);
console.log(...patched);
console.log("restored", len(f32), len(u8));
// Shadow on one kind's prototype only.
Object.defineProperty(Uint8Array.prototype, "length", { get() { return 7; }, configurable: true });
const shadowed = ["kind shadow", len(u8), len(f32), len(new Uint8ClampedArray(3))];
delete Uint8Array.prototype.length;
console.log(...shadowed);
console.log("kind unshadow", len(u8), len(f32));
// A getter that throws, caught around the warmed read.
Object.defineProperty(TA, "length", { get() { throw new Error("no length"); }, configurable: true });
let threw = "did not throw";
try { len(f32); } catch (e) { threw = e.message; }
Object.defineProperty(TA, "length", orig);
console.log("threw", threw);
let after = 0;
for (let i = 0; i < 1000; i++) after += len(f32) + len(f64);
console.log("after", after);
// Non-objects at the site.
for (const v of [null, undefined, 5]) { try { console.log("prim", String(len(v))); } catch (e) { console.log("prim threw", e.constructor.name); } }
// The other three accessors the runtime answers from the header, redefined
// in place on %TypedArray%.prototype and restored.
for (const name of ["byteLength", "byteOffset", "buffer"]) {
  const d = Object.getOwnPropertyDescriptor(TA, name);
  const before = typeof f64[name];
  Object.defineProperty(TA, name, { get() { return name + "!"; }, configurable: true });
  const during = f64[name];
  Object.defineProperty(TA, name, d);
  console.log(name, before, during, typeof f64[name]);
}
