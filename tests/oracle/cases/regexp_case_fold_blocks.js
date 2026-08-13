// Case folding, past the four lines regexp_case_fold_unicode.js pins. Those
// two lines are the RULES — Greek and Cyrillic at +32. These are the members
// that break them, which is the half of a hand-written case table that cannot
// be derived from the rule and so has to be pinned one character at a time.

// U+03C2 GREEK SMALL LETTER FINAL SIGMA is -31 and not -32: it shares the
// capital U+03A3 with U+03C3, which is why the offset run cannot be one range.
console.log(/σ/i.test("Σ"), /ς/i.test("Σ"));
console.log(/[Α-Ω]/i.test("ς"));

// The glyph-variant symbols uppercase to the ordinary capital of the letter
// they vary — U+03D0 GREEK BETA SYMBOL to U+0392, U+03F0 GREEK KAPPA SYMBOL to
// U+039A — so they are not an offset at all.
console.log(/ϐ/i.test("Β"), /ϰ/i.test("Κ"));

// The extended Cyrillic capitals at U+0400 were encoded 0x50 below their small
// letters at U+0450, not 0x20 like the main alphabet at U+0410.
console.log(/ё/i.test("Ё"), /ё/i.test("ё"));

// `g` runs the whole test at every position, and a capital is reached only
// through the reverse direction of the table (22.2.2.7.1).
console.log("Привет".replace(/[а-я]/gi, "."));
console.log("aAΑαАа".replace(/[aαа]/gi, "*"));

// Latin Extended-A is capital/small pairs, except where it is not: U+0131
// LATIN SMALL LETTER DOTLESS I uppercases to ASCII `I`, and 22.2.2.9 step 4
// keeps a non-ASCII character whose uppercase is ASCII — so `/ı/i` does
// NOT match "I", and U+0130 is not its partner.
console.log(/š/i.test("Š"), /ı/i.test("I"));

// Armenian: U+0561..U+0586 against U+0531..U+0556, an offset of 0x30.
console.log(/ա/i.test("Ա"), /[ա-ֆ]/i.test("Ֆ"));

// `\w` under `i` is the one place the flag could change a SET rather than a
// comparison -- and, without `u`, does not: 22.2.2.7.1 step 3 asserts the
// extra word characters are empty unless [[Unicode]] and [[IgnoreCase]] are
// BOTH true. U+017F LATIN SMALL LETTER LONG S and U+212A KELVIN SIGN are the
// two that look like members, and step 4 of Canonicalize is exactly why they
// are not: it keeps a non-ASCII character whose uppercase is ASCII, so neither
// canonicalizes to a word character at all -- and the set therefore agrees
// with the comparison two lines above, which refuses to fold the dotless i for
// the same reason. `cases/regexp_word_class_i` is the whole set question.
console.log(/^\w+$/i.test("ſk"), /^\w+$/.test("ſk"));
console.log(/\w/i.test("K"), /\w/.test("K"));

// A block bronze has no rule for is still a named error and never a guess, and
// the name carries the code point that caused it.
try {
  new RegExp("ẞ", "i");
  console.log("U+1E9E folded, which bronze has no table for");
} catch (e) {
  console.log(e instanceof SyntaxError, e.message.indexOf("U+1E9E") >= 0);
}
