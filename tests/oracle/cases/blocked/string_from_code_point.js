// BLOCKED: `unsupported: String.fromCodePoint is not implemented` — it is on
// builtin_constructors.cpp's unimplemented list for the String constructor.
//
// 22.1.2.2 is CodePointsToString (11.1.6) over the arguments: each code point
// becomes one code unit below 0x10000 and a surrogate PAIR at or above it
// (11.1.3 UTF16EncodeCodePoint), which is what separates it from
// `String.fromCharCode` — 22.1.2.1 truncates to a code unit and cannot spell
// an astral character at all. A code point above 0x10FFFF (or not an integer)
// is a RangeError naming the value (22.1.2.2 step 2.b).
//
// The astral lines below pin the pair by its halves rather than by printing
// it: U+1F600 − 0x10000 = 0xF600, so the high surrogate is
// 0xD800 + (0xF600 >> 10) = 0xD83D = 55357 and the low is
// 0xDC00 + (0xF600 & 0x3FF) = 0xDE00 = 56832, and the string's length is 2
// because length counts code units (6.1.4).
//
// Unblocking this means implementing 22.1.2.2 and taking "fromCodePoint" off
// the String constructor's unimplemented list.
console.log(String.fromCodePoint(65, 66));
console.log(String.fromCodePoint());
console.log(String.fromCodePoint(0x1F600).length);
console.log(String.fromCodePoint(0x1F600).charCodeAt(0));
console.log(String.fromCodePoint(0x1F600).charCodeAt(1));
try {
  String.fromCodePoint(0x110000);
} catch (e) {
  console.log(e instanceof RangeError);
}
