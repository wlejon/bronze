// A loop that only stores into some receivers and reads the others: the typed
// loop pass gives it a copy that takes the stored-to ones as word views
// (Int32Array, Uint32Array, Float32Array, Float64Array) and the rest as
// Arrays. Every other mix has to answer the same through the other copies.
function fill(f, u, index, xs, ys, n, a, b, tag) {
  for (let i = 0; i < n; i++) {
    const x = xs[i * 2];
    const y = xs[i * 2 + 1];
    f[index++] = a * x + b * y;
    f[index++] = ys[i];
    u[index++] = tag;
    u[index++] = (i << 16) | 7;
  }
  return index;
}
function pack(out, index, src, n, off, base) {
  for (let i = 0; i < n; i++) out[index++] = off + src[i + base] - 1;
  return index;
}
function show(name, v) {
  console.log(name, Array.prototype.join.call(v, ','));
}
const xs = [];
const ys = [];
for (let i = 0; i < 12; i++) xs.push(i * 0.5 - 1.25);
for (let i = 0; i < 6; i++) ys.push(i === 3 ? undefined : i / 3);

const buf = new ArrayBuffer(4 * 40);
const f32 = new Float32Array(buf);
const u32 = new Uint32Array(buf);
console.log(fill(f32, u32, 2, xs, ys, 6, 2, -3, 0xfffffff0 + 5));
show('f32/u32', u32);

const f64 = new Float64Array(30);
const i32 = new Int32Array(30);
console.log(fill(f64, i32, 0, xs, ys, 6, 0.1, 0.2, -2.75));
show('f64', f64);
show('i32', i32);

// Stored-to receivers that are Arrays, a Uint16Array (no word view), and one
// view written past its end.
const pa = [];
const pb = [];
console.log(fill(pa, pb, 0, xs, ys, 6, 1, 1, 'tag'));
show('array a', pa);
show('array b', pb);
const u16 = new Uint16Array(26);
const u8 = new Uint8ClampedArray(26);
console.log(fill(u16, u8, 0, xs, ys, 6, 300, 7, 300.6));
show('u16', u16);
show('u8c', u8);
const small = new Float32Array(5);
console.log(fill(small, small, 3, xs, ys, 6, 1, 0, 9));
show('small', small);

// Sources that are views, a string index, a fractional start.
const src = new Float32Array([5, 6, 7, 8, 9, 10]);
const idx = new Uint32Array(8);
console.log(pack(idx, 1, src, 4, 100, 2));
show('idx', idx);
const idx16 = new Uint16Array(8);
console.log(pack(idx16, 0, [70000, 2, 3, 4, 5], 4, 1, 1));
show('idx16', idx16);
const o = [];
console.log(pack(o, '2', [1, 2, 3], 3, 0, 0), o.length);
const frac = new Int32Array(6);
console.log(pack(frac, 0.5, [1, 2, 3], 3, 0, 0));
show('frac', frac);
for (let k = 0; k < 2000; k++) fill(f32, u32, 0, xs, ys, 6, k, 1, k);
show('hot', u32);
