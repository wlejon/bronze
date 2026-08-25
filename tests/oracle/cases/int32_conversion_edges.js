// ECMA-262 7.1.6 ToInt32 / 7.1.7 ToUint32 reached from a PROVEN NUMBER, which
// is the path the backend emits inline (`llvm_convert.cpp emitToInt32F64`):
// a range test, an `fptosi` to i64 and a `trunc` to i32, with the helper kept
// for what the test refuses. `dynamic_int32_wrap.js` pins the same algebra
// through a dynamic operand; this file pins the boundaries of the INLINE
// path's range test, which that one never reaches.
//
// A Float64Array read is the operand form: it is proven f64 without a
// manifest, so the conversion sees a raw double rather than a boxed Value.
//
// The interesting boundaries are NOT the int32 ones. The fast path converts
// through int64, so what decides between it and the helper is +-2^63 — and
// everything between 2^31 and 2^63 is a value the inline path must WRAP
// correctly rather than saturate.
const v = new Float64Array(1);

function row(x) {
  v[0] = x;
  const d = v[0];
  console.log(d | 0, d >>> 0, ~d, d << 0, d >> 0, d & 0xffff, (d | 0) === 0 ? "z" : "n");
}

// int32 boundaries, the 2^32 wrap, and the sign edges around them.
row(0);
row(-0);
row(1);
row(-1);
row(2147483647);
row(2147483648);
row(2147483649);
row(-2147483648);
row(-2147483649);
row(4294967295);
row(4294967296);
row(4294967297);
row(-4294967296);
row(-4294967297);

// Fractions truncate TOWARD ZERO, both signs, on both sides of the int32 edge.
row(0.5);
row(-0.5);
row(0.9999999999);
row(-0.9999999999);
row(2147483647.75);
row(-2147483648.75);
row(-2147483649.5);
row(4294967295.75);
row(-4294967295.75);

// Inside the fast path but far outside int32: exactly representable integers
// at and around 2^52, where doubles stop having a fractional part at all.
row(4503599627370496);      // 2^52
row(4503599627370497);      // 2^52 + 1
row(-4503599627370497);
row(9007199254740991);      // 2^53 - 1
row(9007199254740992);      // 2^53
row(1234567890123);
row(-1234567890123);

// The fast path's own boundary: 2^63 and the largest double below it. The
// helper owns everything at or past it, and must agree with the inline path
// on the near side.
row(9223372036854774784);   // 2^63 - 1024, the largest double < 2^63
row(-9223372036854774784);
row(9223372036854775808);   // 2^63 exactly — helper
row(-9223372036854775808);  // -2^63 exactly — INSIDE the fast path
row(18446744073709551616);  // 2^64 — helper
row(-18446744073709551616);

// No integer part at all.
row(NaN);
row(Infinity);
row(-Infinity);
row(1e21);
row(1e100);
row(-1e100);
row(5e-324);
row(-5e-324);

// The same values through a shift COUNT, which is ToUint32 masked to five
// bits, so the conversion feeds an operand the shift then reduces again.
function counts(n) {
  v[0] = n;
  const d = v[0];
  return [1 << d, 256 >> d, -1 >>> d].join(",");
}
console.log(counts(0), counts(31), counts(32), counts(33));
console.log(counts(-1), counts(4294967296), counts(4294967297), counts(NaN));
console.log(counts(9223372036854775808), counts(-9223372036854775808));

// And through arithmetic, where the operand is a computed double rather than
// a load: the same wraps must come out of a value LLVM can see the shape of.
function wrap(a, b) {
  v[0] = a;
  v[1 - 1] = v[0] * b;
  return v[0] | 0;
}
console.log(wrap(65536, 65536), wrap(65536, 65537), wrap(-65536, 65537));
console.log(wrap(3, 1e30), wrap(1e300, 1e300), wrap(0, Infinity));

// Uint8ClampedArray rounds half to even instead of truncating, and shares the
// conversion site's neighbourhood in the emitter — pinned so a change to one
// cannot silently move the other.
const c = new Uint8ClampedArray(8);
const clamps = [-1, -0.5, 0, 0.5, 1.5, 2.5, 254.5, 255.5, 300, NaN, 1e300, -1e300];
console.log(clamps.map((x) => { c[0] = x; return c[0]; }).join(","));

// Int8/Int16/Int32 stores take the same conversion and then narrow.
const i8 = new Int8Array(1);
const i16 = new Int16Array(1);
const i32 = new Int32Array(1);
const u32 = new Uint32Array(1);
const wide = [0, -1, 127, 128, 32767, 32768, 2147483648, 4294967296, 4294967297,
              -2147483649, 1e18, NaN, Infinity, -0.5, 2.9, -2.9];
for (const x of wide) {
  i8[0] = x; i16[0] = x; i32[0] = x; u32[0] = x;
  console.log(i8[0], i16[0], i32[0], u32[0]);
}
