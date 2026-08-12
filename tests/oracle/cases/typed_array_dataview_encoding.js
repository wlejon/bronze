// What a `DataView` accessor actually writes: ECMA-262 25.3.1.5
// NumericToRawBytes and 25.3.1.6 RawBytesToNumeric, checked against the BYTES
// rather than against a value read back through the same code that wrote it.
//
// Every expectation below is two derivations meeting. The first is the value ->
// bits half: Table 70's conversion operation for an integer type (ToInt8 ..
// ToUint32, which are a MODULO and never a clamp or a rejection), and the
// IEEE-754 binary32/binary64 encoding with roundTiesToEven for a float. The
// second is the bits -> bytes half, which is `isLittleEndian` and nothing else:
// big-endian, the default, puts the most significant byte at the lowest
// address.
//
// Reading the bytes through a `Uint8Array` over the same buffer is what makes
// the check independent. A DataView asked to read back what it just wrote would
// agree with itself under any byte order at all — including the host's, which
// is the one answer 25.3 must never give.
//
// Three things this case pins that are easy to get subtly wrong:
//
//  - Table 70 has no clamped entry. `setUint8(0, -1.7)` is ToUint8, which
//    truncates and takes the modulo, so it stores 255; a Uint8ClampedArray
//    element would store 0. No DataView accessor clamps.
//  - A NaN written through `setFloat32` / `setFloat64` has ONE encoding here.
//    25.3.1.5 allows any NaN pattern, which is a freedom the deterministic-
//    output rule cannot take twice, so the quiet NaN of each width is written
//    explicitly rather than left to whatever the compiler's `(float)NaN` is.
//  - `0.1` round-trips through Float64 and does not through Float32. The bytes
//    of both are pinned, so the pair says WHY: 0x3FB999999999999A is the
//    nearest double and 0x3DCCCCCD the nearest float, and re-widening the
//    second is a different number.

const buffer = new ArrayBuffer(16);
const view = new DataView(buffer);
const bytes = new Uint8Array(buffer);

// The buffer's bytes, read through the typed array — the independent witness.
function bytesAt(from, count) {
  let out = "";
  for (let i = 0; i < count; i++) {
    if (i) out += " ";
    out += bytes[from + i];
  }
  return out;
}

function clear() {
  for (let i = 0; i < 16; i++) bytes[i] = 0;
}

// --- Table 70's conversion operations: a modulo, on a truncated value.
view.setInt8(0, 300);
console.log(view.getInt8(0), view.getUint8(0));
view.setInt8(0, -1);
console.log(view.getInt8(0), view.getUint8(0));
view.setUint8(0, -1);
console.log(view.getInt8(0), view.getUint8(0));
view.setUint8(0, 1.7);
console.log(view.getUint8(0));
view.setUint8(0, -1.7);
console.log(view.getUint8(0));
view.setUint16(0, 65536);
console.log(view.getUint16(0));
view.setUint16(0, -1);
console.log(view.getUint16(0), view.getInt16(0));
view.setInt16(0, 32768);
console.log(view.getInt16(0));
view.setInt32(0, 2147483648);
console.log(view.getInt32(0), view.getUint32(0));
view.setInt32(0, 2147483649);
console.log(view.getInt32(0));
view.setUint32(0, -1);
console.log(view.getUint32(0), view.getInt32(0));
view.setUint32(0, 4294967301);
console.log(view.getUint32(0));
view.setInt32(0, NaN);
console.log(view.getInt32(0));
view.setInt32(0, Infinity);
console.log(view.getInt32(0));
view.setInt32(0, -Infinity);
console.log(view.getInt32(0));

// --- IEEE-754, both widths, both orders.
clear();
view.setFloat32(0, 1.5);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat32(0, 1.5, true);
console.log(bytesAt(0, 4), view.getFloat32(0, true));
clear();
view.setFloat64(0, 1.5);
console.log(bytesAt(0, 8), view.getFloat64(0));
clear();
view.setFloat64(0, 1.5, true);
console.log(bytesAt(0, 8), view.getFloat64(0, true));

// --- NaN, the infinities, and the two ends of the float32 range.
clear();
view.setFloat32(0, NaN);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat64(0, NaN);
console.log(bytesAt(0, 8), view.getFloat64(0));
clear();
view.setFloat32(0, Infinity);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat32(0, -Infinity);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat64(0, Infinity);
console.log(bytesAt(0, 8), view.getFloat64(0));
clear();
view.setFloat64(0, -Infinity);
console.log(bytesAt(0, 8), view.getFloat64(0));
// Overflow of the narrowing is an infinity, and underflow a zero of the right
// sign — both are roundTiesToEven's answers and neither is an error.
clear();
view.setFloat32(0, 1e39);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat32(0, -1e39);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat32(0, 1e-50);
console.log(bytesAt(0, 4), view.getFloat32(0));

// --- The signed zero survives, which is the one value `=== 0` cannot tell
// apart from its own negation.
clear();
view.setFloat64(0, -0);
console.log(bytesAt(0, 8), Object.is(view.getFloat64(0), -0));
clear();
view.setFloat32(0, -0);
console.log(bytesAt(0, 4), Object.is(view.getFloat32(0), -0));

// --- The same number at two widths.
clear();
view.setFloat32(0, 0.1);
console.log(bytesAt(0, 4), view.getFloat32(0) === 0.1);
clear();
view.setFloat64(0, 0.1);
console.log(bytesAt(0, 8), view.getFloat64(0) === 0.1);

// --- Unaligned, at every width. There is no alignment requirement in 25.3 at
// all: an offset is a byte position, and an odd one is as ordinary as an even
// one. This is the property no %TypedArray% element access has.
clear();
view.setInt16(1, -2);
console.log(bytesAt(0, 4), view.getInt16(1));
clear();
view.setInt32(3, -2);
console.log(bytesAt(2, 6), view.getInt32(3));
clear();
view.setUint32(5, 16909060);
console.log(bytesAt(5, 4), view.getUint32(5), view.getUint32(5, true));
clear();
view.setFloat32(7, 1.5);
console.log(bytesAt(7, 4), view.getFloat32(7));
clear();
view.setFloat64(5, 1.5, true);
console.log(bytesAt(5, 8), view.getFloat64(5, true));

// --- The round trip of every getter/setter pair, at both orders.
//
// A one-byte type has no byte order to have, and the flag is ignored rather
// than rejected — which is worth pinning, because "there is nothing to reverse"
// and "the argument is not allowed" are different claims.
clear();
view.setInt8(0, -128);
console.log(bytesAt(0, 1), view.getInt8(0), view.getInt8(0, true));
view.setUint8(0, 200);
console.log(bytesAt(0, 1), view.getUint8(0), view.getUint8(0, true));

clear();
view.setInt16(0, -2, true);
console.log(bytesAt(0, 2), view.getInt16(0, true), view.getInt16(0));
clear();
view.setInt16(0, -2);
console.log(bytesAt(0, 2), view.getInt16(0), view.getInt16(0, true));
clear();
view.setUint16(0, 4660);
console.log(bytesAt(0, 2), view.getUint16(0), view.getUint16(0, true));
clear();
view.setUint16(0, 4660, true);
console.log(bytesAt(0, 2), view.getUint16(0, true), view.getUint16(0));

clear();
view.setInt32(0, -2, true);
console.log(bytesAt(0, 4), view.getInt32(0, true), view.getInt32(0));
clear();
view.setInt32(0, -2);
console.log(bytesAt(0, 4), view.getInt32(0), view.getInt32(0, true));
clear();
view.setUint32(0, 16909060);
console.log(bytesAt(0, 4), view.getUint32(0), view.getUint32(0, true));
clear();
view.setUint32(0, 16909060, true);
console.log(bytesAt(0, 4), view.getUint32(0, true), view.getUint32(0));

clear();
view.setFloat32(0, -1.5);
console.log(bytesAt(0, 4), view.getFloat32(0));
clear();
view.setFloat32(0, -1.5, true);
console.log(bytesAt(0, 4), view.getFloat32(0, true));
clear();
view.setFloat64(0, -1.5);
console.log(bytesAt(0, 8), view.getFloat64(0));
clear();
view.setFloat64(0, -1.5, true);
console.log(bytesAt(0, 8), view.getFloat64(0, true));

// --- Step 5 of both operations is ToBoolean(isLittleEndian), not a test for
// `true`. So `1` reverses the bytes and `""` and `null` do not, and an omitted
// argument is `undefined`, which is false — which is the whole of "the default
// is big-endian".
clear();
view.setUint16(0, 4660, 1);
console.log(bytesAt(0, 2));
view.setUint16(0, 4660, "");
console.log(bytesAt(0, 2));
view.setUint16(0, 4660, null);
console.log(bytesAt(0, 2));
view.setUint16(0, 4660, "false");
console.log(bytesAt(0, 2));
