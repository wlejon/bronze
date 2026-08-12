// BLOCKED: `unsupported: case-insensitive matching of U+03A9 (bronze carries
// no Unicode case tables; only ASCII and Latin-1 fold under the `i` flag)`,
// named at the literal by src/regex/parser.cpp.
//
// 22.2.2.9 Canonicalize is defined by toUppercase from the Unicode Default Case
// Conversion table, and bronze implements exactly the part of that table it can
// state in code and check by hand: ASCII, and the Latin-1 supplement with its
// two escapes from it (U+00B5 and U+00FF uppercase OUT of Latin-1, U+00DF's
// uppercase is two units so it stays put). Everything above is refused rather
// than guessed, because a wrong fold is invisible: `/\u03a9/i` silently not
// matching a lowercase omega is a wrong answer a test would only catch if
// someone thought to write it.
//
// The refusal is at COMPILE time and by code point, so it costs nothing to
// programs that do not use `i` over cased scripts — a pattern of CJK or emoji
// still takes `i` — and it is deliberately conservative: whole blocks that
// merely CONTAIN cased characters are refused, not just the characters
// themselves.
//
// This lands with the table generated from UnicodeData.txt that
// regexp_unicode_flag also needs; the two share the data even though they are
// different features.
//
// What this case pins when it lands, from 22.2.2.9 Canonicalize and
// 22.2.2.7.1 (a class matches when the SET holds a member canonicalizing to
// the same code point the input does, which is what makes a RANGE fold):
console.log(/\u03a9/i.test("\u03c9"));
console.log(/\u0410/i.test("\u0430"));
console.log(/[\u03b1-\u03c9]/i.test("\u0393"));
console.log("\u0391\u0392".replace(/[\u03b1-\u03c9]/gi, "-"));
