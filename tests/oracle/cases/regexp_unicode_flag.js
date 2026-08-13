// The `u` flag and the property escapes only it can spell, pinned together
// because they are one piece of work rather than two: 22.2.1's grammar takes a
// +UnicodeMode parameter, and `\p{...}` is one of the productions that
// parameter turns on.
//
// The alphabet is what everything else follows from. Without `u` the matcher's
// character is the UTF-16 code unit, which is what 22.2.1 specifies; with `u`
// it is the code point, and so `.` and a character class consume a surrogate
// PAIR as one character, AdvanceStringIndex (22.2.7.3) steps two units across
// one, `\u{...}` becomes spellable, a range may span above U+FFFF, and the
// Annex B leniencies switch off so that `{` and a lone `]` are syntax errors
// instead of literal characters.
//
// `\p{...}` reads a real table, because 22.2.1's UnicodePropertyValueExpression
// is defined by reference to UAX #44 and nothing less will answer it.
// tools/gen_unicode_tables.py writes src/regex/unicode_data_gc.cpp from the
// UCD, once, and the result is an ordinary checked-in source: the build never
// runs Python. `\p{L}` below is the union of the five letter categories and
// `\p{Nd}` is one of the number categories, both derived from the same runs
// rather than listed a second time.
//
// What this case pins, from 22.2.1 with the +UnicodeMode parameter, 22.2.7.3
// AdvanceStringIndex, and 22.2.6.18 (`unicode`):
console.log(/\p{L}+/u.exec("42 h\u00e9llo!")[0].length);
console.log(/\p{Nd}/u.test("\u0663"), /\p{Nd}/u.test("x"));

// One astral code point is ONE character under `u` and two units without it.
console.log(/^.$/u.test("\u{1F600}"), /^.$/.test("\u{1F600}"));
console.log(/\u{1F600}/u.test("a\u{1F600}b"));
console.log(/[\u{1F600}-\u{1F64F}]/u.test("\u{1F607}"));
console.log("a\u{1F600}b".replace(/./gu, "-"));
