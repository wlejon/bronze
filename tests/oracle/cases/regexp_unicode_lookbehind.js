// Lookbehind under `u`: 22.2.2.6's `direction` and 22.2.1's +UnicodeMode
// alphabet meeting, which is the one place where getting either alone right is
// not enough.
//
// A backward atom reads the character BEFORE the position. Without `u` that is
// the code unit at `pos - 1`; with `u` it is the code POINT ending there, which
// means recognising a trailing surrogate and stepping back over its lead. A
// backward read that took `pos - 1` alone would answer with the trailing half
// of an astral character and leave the position one unit too high for whatever
// precedes it — a wrong answer on any lookbehind over astral text, and a silent
// one, since the assertion consumes nothing and reports no extent of its own.
//
// Every line below is written so that the unit reading and the code point
// reading disagree.

// The plain case, and its discriminator: `\uDE00` is the trailing half of this
// character, so a unit reading matches it and a code point reading does not.
console.log(/(?<=\u{1F600})x/u.test("\u{1F600}x"));
console.log(/(?<=\uDE00)x/u.test("\u{1F600}x"), /(?<=\uDE00)x/.test("\u{1F600}x"));

// Where the position LANDS, caught by an anchor: one character back from the
// `x` is the start of the string under `u` and the middle of a surrogate pair
// without it.
console.log(/(?<=^.)x/u.test("\u{1F600}x"), /(?<=^.)x/.test("\u{1F600}x"));

// A negative lookbehind is the same body, so it follows.
console.log(/(?<!\u{1F600})x/u.test("\u{1F600}x"), /(?<!\uDE00)x/u.test("\u{1F600}x"));

// The match itself is still reported in code UNITS: the `x` is at index 2.
console.log("\u{1F600}x".search(/(?<=\u{1F600})x/u));

// 22.2.2.8 orders a capture closed backward, and what it captured is a whole
// character under `u` and one surrogate without it.
console.log(/(?<=(.))x/u.exec("\u{1F600}x")[1].length, /(?<=(.))x/.exec("\u{1F600}x")[1].length);

// A counted quantifier run backward walks CHARACTERS. Three astral characters
// precede the `x`, and one of them has to be the `\u{1F600}` the pattern names
// first — so `.{2}` fits and `.{3}` cannot. Counting units instead would make
// `.{4}` the one that fits, which is the last line.
console.log(/(?<=\u{1F600}.{2})x/u.test("\u{1F600}\u{1F600}\u{1F600}x"));
console.log(/(?<=\u{1F600}.{3})x/u.test("\u{1F600}\u{1F600}\u{1F600}x"));
console.log(/(?<=\uD83D\uDE00.{4})x/.test("\u{1F600}\u{1F600}\u{1F600}x"));

// Lazy backward takes the fewest turns it can and still lands on a boundary;
// greedy backward gives them back one character at a time, which `^` measures.
console.log(/(?<=\u{1F600}{1,2}?)x/u.test("\u{1F600}\u{1F600}\u{1F600}x"));
console.log(/(?<=^\u{1F600}{1,2})x/u.test("\u{1F600}\u{1F600}\u{1F600}x"));
console.log(/(?<=^\u{1F600}{1,3})x/u.test("\u{1F600}\u{1F600}\u{1F600}x"));

// A lookbehind that precedes an astral atom: the scan never begins between the
// halves of a pair, so the match index is 2 and not 1.
console.log(/(?<=\u{1F600})\u{1F600}/u.exec("\u{1F600}\u{1F600}").index);
