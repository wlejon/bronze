// `ArrayBuffer` and the views over it (ECMA-262 25.1 and 23.2.5.1 step 6).
// Two views over one buffer address the SAME bytes, so a write through either
// is visible through the other — that is what makes a buffer a buffer, and it
// is what three.js's `extras/DataUtils.js` uses to read a float's bit pattern.
//
// The byte ORDER is the platform's: 10.4.5.5 stores an element with
// SetValueInBuffer(..., isLittleEndian = the implementation's choice), and
// only `DataView` lets a program name it. Every target bronze builds for is
// little-endian, so that is what the numbers below assume — a divergence
// recorded in docs/0029 rather than one this case discovers.
const buf = new ArrayBuffer(8);
console.log(buf.byteLength);

const bytes = new Uint8Array(buf);
const words = new Uint32Array(buf);
console.log(bytes.length, words.length);
console.log(bytes.buffer === buf, words.buffer === buf);

bytes[0] = 1;
bytes[1] = 2;
bytes[2] = 3;
bytes[3] = 4;
// 0x04030201, little end first.
console.log(words[0]);

words[1] = 16909060; // 0x01020304
console.log(bytes[4], bytes[5], bytes[6], bytes[7]);

// A view at an OFFSET, overlapping both of the above: bytes 2..5.
const half = new Uint16Array(buf, 2, 2);
console.log(half.length, half.byteOffset, half.byteLength);
// bytes 2 and 3 are 3 and 4, so the first element is 0x0403.
console.log(half[0]);
half[1] = 65535;
console.log(bytes[4], bytes[5], bytes[6]);
console.log(words[1]);

// 23.2.5.1 step 6.d: omitting the length spans the rest of the buffer, and
// the remainder has to divide evenly by the element width.
const tail = new Uint16Array(buf, 4);
console.log(tail.length, tail.byteOffset);

// A buffer of zero bytes is legal (25.1.3.1 with byteLength 0) and so is a
// view over it; every derived count is 0 and nothing about it is a special
// case.
const empty = new ArrayBuffer(0);
console.log(empty.byteLength);
const noneAtAll = new Float64Array(empty);
console.log(noneAtAll.length, noneAtAll.byteLength, noneAtAll.byteOffset);
console.log(noneAtAll);

// The three RangeErrors 23.2.5.1 names, each for a different reason: an
// offset that is not a multiple of the element width (step 6.b), a length
// that does not divide the remaining bytes (step 6.d.i), and a window that
// runs past the end of the buffer (step 6.e.iii).
try {
  new Float32Array(buf, 2);
} catch (e) {
  console.log(e.name);
}
try {
  new Float32Array(new ArrayBuffer(6));
} catch (e) {
  console.log(e.name);
}
try {
  new Float32Array(buf, 4, 4);
} catch (e) {
  console.log(e.name);
}

// A view built from another typed array COPIES; a view built from that
// array's BUFFER aliases. The two spellings are one character apart and mean
// opposite things, so both are pinned here.
const source = new Float32Array([1, 2]);
const copied = new Float32Array(source);
const aliased = new Float32Array(source.buffer);
source[0] = 99;
console.log(copied[0], aliased[0]);
