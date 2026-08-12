// A character outside the BMP is TWO UTF-16 code units, and every unit-indexed
// operation says so: `length` counts units, and charCodeAt yields the high and
// low surrogate separately.

const emoji = "🌍";
const greeting = "Hello " + emoji;

console.log(emoji.length);
console.log(emoji.charCodeAt(0));
console.log(emoji.charCodeAt(1));
console.log(greeting.length);
console.log(greeting.charCodeAt(6));
console.log(greeting.charCodeAt(7));

// codePointAt is the member that reads the PAIR (22.1.3.4, via 11.1.4
// CodePointAt), and it is the one string operation above that does not count
// in units. U+1F30D encodes as 0xD83C 0xDF0D, so:
//   (0xD83C - 0xD800) * 0x400 + (0xDF0D - 0xDC00) + 0x10000
//   = 60 * 1024 + 781 + 65536 = 127757 = 0x1F30D
console.log(emoji.codePointAt(0));
console.log(greeting.codePointAt(6));

// Read at the SECOND half of the pair and 11.1.4 returns that unit alone: the
// position holds a trailing surrogate, which is never the start of a pair. It
// is the same 57101 charCodeAt gave above, and the reason walking a string by
// code point has to advance by 2 over an astral character rather than by 1.
console.log(emoji.codePointAt(1));

// An UNPAIRED leading surrogate is its own code point. charAt yields one unit,
// so this string is a lone 0xD83C with nothing after it — 11.1.4's "position +
// 1 = size" branch, which returns the unit rather than reading past the end.
console.log(emoji.charAt(0).codePointAt(0));

// Out of range is `undefined`, not the NaN charCodeAt answers for the same
// position (22.1.3.4 step 4). Both spellings of "past the end" agree.
console.log(emoji.codePointAt(2));
console.log(greeting.codePointAt(99));

// A BMP character is one unit and one code point, so the two members agree.
console.log(greeting.codePointAt(0));
console.log(greeting.charCodeAt(0));
