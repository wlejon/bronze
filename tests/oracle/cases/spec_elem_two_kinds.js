// Element reads and writes at sites that meet two typed-array kinds: Float32
// and Float64 (a read joins two doubles; a Float32 store rounds), Int32 and
// Uint32 (written only: their reads load as different types), Uint16 and
// Int16 (read as int32s, stored as their low bits), Uint8 and Uint8Clamped
// (a store wraps in one and clamps in the other).
"use strict";
function lerp(out, a, b, t) {
  for (let i = 0; i < out.length; i++) out[i] = a[i] + (b[i] - a[i]) * t;
  return out;
}
// One copy per pair of kinds, so each site meets two.
function copyWide(dst, src) {
  for (let i = 0; i < src.length; i++) dst[i] = src[i] * 3 - 7;
}
function copyShort(dst, src) {
  for (let i = 0; i < src.length; i++) dst[i] = src[i] * 3 - 7;
}
function copyByte(dst, src) {
  for (let i = 0; i < src.length; i++) dst[i] = src[i] * 3 - 7;
}
function sum(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i];
  return s;
}
function sumShort(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i];
  return s;
}
function sumByte(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i];
  return s;
}
function main() {
  const f32 = [new Float32Array(4), new Float32Array([0.1, 0.2, 0.3, 0.4])];
  const f64 = [new Float64Array(4), new Float64Array([1.1, 2.2, 3.3, 4.4])];
  const i32 = new Int32Array(4), u32 = new Uint32Array(4);
  const u16 = new Uint16Array(4), i16 = new Int16Array(4);
  const u8 = new Uint8Array(4), c8 = new Uint8ClampedArray(4);
  let total = 0;
  for (let r = 0; r < 4000; r++) {
    const t = (r % 10) / 10;
    const out = r & 1 ? f32[0] : f64[0];
    const a = r & 2 ? f32[1] : f64[1];
    const b = r & 4 ? f64[1] : f32[1];
    lerp(out, a, b, t);
    total += sum(out);
    const src = [r, -r, r * 1000, 0x7fffffff - r];
    copyWide(r & 1 ? i32 : u32, src);
    copyShort(r & 2 ? u16 : i16, src);
    copyByte(r & 4 ? u8 : c8, src);
    total += sumShort(r & 2 ? u16 : i16) + sumByte(r & 4 ? u8 : c8);
  }
  console.log(total);
  console.log(Array.from(f32[0]).join(","), Array.from(f64[0]).join(","));
  console.log(i32.join(","), u32.join(","), u16.join(","), i16.join(","), u8.join(","), c8.join(","));
}
main();
