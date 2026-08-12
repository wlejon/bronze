// `DataView` (ECMA-262 25.3), which was NOT built. It is the one view that
// reads a buffer at an arbitrary byte offset with an explicitly named byte
// order, which is exactly what makes it a separate object rather than a tenth
// element kind: `getFloat32(1, true)` has no alignment requirement and no fixed
// endianness, so it cannot share the offset-times-width addressing every
// %TypedArray% element access is.
//
// Today `DataView` is not on lowering's provided-globals list, so the name is
// an unresolved one: a compile-time warning and a `ReferenceError` where it is
// evaluated. That is a named diagnosis and not a silent wrong answer, which is
// why this is a blocked case and not a bug.
//
// Every number below comes from 25.3.1.1..25.3.1.5 (GetViewValue and
// SetViewValue over RawBytesToNumeric / NumericToRawBytes) and the IEEE-754
// encodings those cite; `isLittleEndian` DEFAULTS TO FALSE, which makes
// DataView the one place in the language whose byte order is not the
// platform's.
const buffer = new ArrayBuffer(8);
const view = new DataView(buffer);

console.log(view.byteLength, view.byteOffset);
console.log(view.buffer === buffer);

// Bytes 0x01 0x02 read big-endian are 0x0102, and little-endian are 0x0201.
view.setUint8(0, 1);
view.setUint8(1, 2);
console.log(view.getUint16(0));
console.log(view.getUint16(0, true));

// 0x3F800000, big end first, is the single-precision 1.
view.setUint8(0, 63);
view.setUint8(1, 128);
view.setUint8(2, 0);
view.setUint8(3, 0);
console.log(view.getFloat32(0));

// -2 as an Int16 is 0xFFFE, and big-endian puts 0xFF first.
view.setInt16(4, -2);
const bytes = new Uint8Array(buffer);
console.log(bytes[4], bytes[5]);
console.log(view.getInt16(4), view.getUint16(4));

// 1.5 is 0x3FF8000000000000, so writing it little-endian puts 0x3F last.
view.setFloat64(0, 1.5, true);
console.log(bytes[7], bytes[6], bytes[0]);
console.log(view.getFloat64(0, true));

// A windowed view carries its own offset and length (25.3.4.2, 25.3.4.3),
// which are its slots and not the buffer's.
const windowed = new DataView(buffer, 4, 2);
console.log(windowed.byteLength, windowed.byteOffset);
