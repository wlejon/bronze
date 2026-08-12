// `DataView` (ECMA-262 25.3): a window on an ArrayBuffer whose accessors name a
// width and a byte order per CALL. That is what makes it a separate object
// rather than a tenth element kind — `getFloat32(1, true)` has no alignment
// requirement and no fixed endianness, so it cannot share the offset-times-width
// addressing every %TypedArray% element access is.
//
// Every number below comes from 25.3.1.1..25.3.1.6 (GetViewValue and
// SetViewValue over RawBytesToNumeric / NumericToRawBytes) and the IEEE-754
// encodings those cite. What it pins:
//
// 1. The default slots of `new DataView(buffer)`: the whole buffer, at offset
//    zero, and `view.buffer` is the SAME object that was handed in — not a copy
//    of it, which is what makes two views over one buffer see each other.
// 2. `isLittleEndian` DEFAULTS TO FALSE. `getUint16(0)` over the bytes 0x01
//    0x02 is 258 and `getUint16(0, true)` is 513, and the pair is the point:
//    DataView is the one place in the language whose byte order is not the
//    platform's, so a host that assembled these with a native load would get
//    the first of them wrong and never know.
// 3. A float read out of bytes written one at a time: 0x3F800000 big end first
//    is the single-precision 1, so the encoding and the byte order are both
//    being checked at once.
// 4. A signed write read back three ways — through a Uint8Array over the same
//    buffer, and as Int16 and Uint16 through the DataView. The typed array is
//    what proves the bytes really landed in the buffer and in that order,
//    rather than the DataView agreeing with itself.
// 5. A little-endian Float64 write, whose bytes are checked at both ends of the
//    window: 1.5 is 0x3FF8000000000000, so little-endian puts 0x3F last.
// 6. A WINDOWED view (25.3.4.2, 25.3.4.3): `byteOffset` and `byteLength` are
//    its own slots and not the buffer's.
//
// The error ladder, the round trip of all sixteen accessors and the two views
// observing each other are in `typed_array_dataview_edges`; this case is the
// shape of the object and the byte order it defaults to.
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
