// The RegExp `u` flag: 22.2.1's grammar with the +UnicodeMode parameter, and
// the one change everything here follows from — the matcher's alphabet is the
// CODE POINT rather than the UTF-16 code unit.
//
// Without `u` a surrogate pair is two characters and every `.`, class member
// and cursor step moves one unit; with `u` the pair is one character, `\u{...}`
// is spellable, a class range may span above U+FFFF, and AdvanceStringIndex
// (22.2.7.3) steps two units across a pair. The index a match REPORTS is a code
// unit index in both modes — that never changes — which is why the interesting
// answers below are lengths and positions rather than the matched text.
//
// Lookbehind under `u` is the direction and the alphabet meeting, and is its
// own case (regexp_unicode_lookbehind). The constructs `u` refuses rather than
// enables are regexp_unicode_refusals.

// `.` consumes one code point under `u` and one code unit without it.
console.log(/^.$/u.test("\u{1F600}"), /^.$/.test("\u{1F600}"));

// `\u{...}` is spellable only under `u`, and names the whole character. The
// index it reports is still a unit index: the emoji begins at unit 1.
console.log(/\u{1F600}/u.test("a\u{1F600}b"), "a\u{1F600}b".search(/\u{1F600}/u));

// An astral character written directly in the PATTERN is one atom under `u`, so
// the `+` repeats the whole of it. Without `u` the same text is two atoms and
// the `+` repeats only the trailing surrogate, which no anchored match survives.
console.log(new RegExp("^\u{1F600}+$", "u").test("\u{1F600}\u{1F600}"));
console.log(new RegExp("^\u{1F600}+$").test("\u{1F600}\u{1F600}"));

// The same character written the three other ways a pattern can say it: raw in
// the literal's own source text, as `\u{...}`, and — 22.2.1's
// RegExpUnicodeEscapeSequence — as a lead-surrogate escape immediately followed
// by a trail-surrogate escape, which is ONE code point under `u`.
console.log(/^😀$/u.test("\u{1F600}"));
console.log(/^\u{1F600}$/u.test("\u{1F600}"), /^\uD83D\uDE00$/u.test("\u{1F600}"));

// A class range may span above U+FFFF, as one interval — not as a set of
// surrogate halves, which would also match every other astral character
// sharing a lead.
console.log(/[\u{1F600}-\u{1F64F}]/u.test("\u{1F607}"), /[\u{1F600}-\u{1F64F}]/u.test("\u{1F650}"));

// A negated class covers the astral plane because 22.2.2.7.1 inverts the ANSWER
// after a membership test the code point simply fails.
console.log(/^[^x]$/u.test("\u{1F600}"), /^[^x]$/.test("\u{1F600}"));

// `\D`, `\W` and `\S` are built by COMPLEMENTING their sets, so their ceiling is
// the mode's: a complement that stopped at U+FFFF would exclude every astral
// character from a set whose whole meaning is "not a digit".
console.log(/^\D$/u.test("\u{1F600}"), /^\W$/u.test("\u{1F600}"), /^\S$/u.test("\u{1F600}"));
console.log(/^\D$/.test("\u{1F600}"));

// An unpaired surrogate is still a character in its own right and still matches
// as itself. The trailing half of a REAL pair is not, because it is not a
// position AdvanceStringIndex ever visits under `u`.
console.log(/^.$/u.test("\uD83D"), /^.$/u.test("\uDE00"));
console.log(/\uDE00/u.test("\u{1F600}"), /\uDE00/.test("\u{1F600}"));

// AdvanceStringIndex, seen from every member that steps a cursor. "a<emoji>b"
// is three characters and four code units.
console.log("a\u{1F600}b".replace(/./gu, "-"), "a\u{1F600}b".replace(/./g, "-"));
console.log("a\u{1F600}b".match(/./gu).length, "a\u{1F600}b".match(/./g).length);
// An EMPTY match steps by the same operation: four match positions under `u`
// (0, 1, 3, 4) against five without it, so the results differ by one dash and
// nothing else.
console.log("a\u{1F600}b".replace(/(?:)/gu, "-").length,
            "a\u{1F600}b".replace(/(?:)/g, "-").length);
console.log("a\u{1F600}b".split(/(?:)/u).length, "a\u{1F600}b".split(/(?:)/).length);

// `lastIndex` after each `exec`, which is where the step is visible directly.
const cursor = /./gu;
const text = "a\u{1F600}b";
cursor.exec(text);
console.log(cursor.lastIndex);
cursor.exec(text);
console.log(cursor.lastIndex);
cursor.exec(text);
console.log(cursor.lastIndex);
console.log(cursor.exec(text), cursor.lastIndex);

// 22.2.6.5 puts `u` between `s` and `y`, and 22.2.6.18 makes it readable.
console.log(/a/gu.flags, /a/yu.flags, /a/gu.unicode, /a/g.unicode);
console.log(String(/ab/gu));
