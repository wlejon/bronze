// BLOCKED: `unsupported: the RegExp `u` flag is not implemented (bronze
// matches per UTF-16 code unit)` and, behind it, `unsupported: unicode
// property escapes `\p{...}` are not implemented`.
//
// Both are named at the literal by src/regex/parser.cpp, and they are one
// piece of work rather than two. docs/0024 decided the matcher's alphabet is
// the UTF-16 code unit, which is what 22.2.1 specifies WITHOUT `u`; the `u`
// flag changes the alphabet to the code point, and with it: `.` and a
// character class consume a surrogate PAIR as one character, AdvanceStringIndex
// (22.2.7.3) steps by two units across one, `\u{...}` becomes spellable, a
// range may span above U+FFFF, and the Annex B leniencies switch off so that
// `{` and a lone `]` become syntax errors instead of literal characters.
//
// `\p{...}` is refused separately but lands with it: property escapes are only
// legal in UnicodeMode, and each one needs a real table of Unicode General
// Category and Script ranges — 22.2.1's UnicodePropertyValueExpression is
// defined by reference to UAX #44, which is data bronze does not carry (the
// same reason case folding above U+00FF is refused). A generated table module
// is the honest way in, and it is big enough to be its own chunk.
//
// What this case pins when it lands, from 22.2.1 with the +UnicodeMode
// parameter, 22.2.7.3 AdvanceStringIndex, and 22.2.6.18 (`unicode`):
console.log(/\p{L}+/u.exec("42 h\u00e9llo!")[0].length);
console.log(/\p{Nd}/u.test("\u0663"), /\p{Nd}/u.test("x"));

// One astral code point is ONE character under `u` and two units without it.
console.log(/^.$/u.test("\u{1F600}"), /^.$/.test("\u{1F600}"));
console.log(/\u{1F600}/u.test("a\u{1F600}b"));
console.log(/[\u{1F600}-\u{1F64F}]/u.test("\u{1F607}"));
console.log("a\u{1F600}b".replace(/./gu, "-"));
