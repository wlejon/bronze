// `\p{...}` and `\P{...}`: 22.2.1's UnicodePropertyValueExpression, over the
// only property bronze carries a table for.
//
// The table is generated. tools/gen_unicode_tables.py reads the UCD once and
// writes src/regex/unicode_data_gc.cpp; the build never runs Python, and the
// generated file is an ordinary checked-in source like any other. What it
// holds is the General_Category of every code point, as the runs the property
// forms, so a lookup is a binary search and no code point can be missing from
// it or claimed by two categories.
//
// Three things follow from that shape and are pinned below. A union is the
// union of its MEMBERS rather than a second set of ranges, so `\p{L}` and
// `\p{Lu}` cannot drift apart. `Cn` is a category like any other, so
// unassigned code points are nameable and `Assigned` is a complement rather
// than a table. And `\P` complements over the CODE POINT ceiling, because
// `\p{...}` is spellable only under `u` and there is no other ceiling it could
// mean.
//
// Every character above U+007F is spelled by escape: the whole content of some
// lines below is which category one obscure code point is in, and no reader
// should have to take a glyph's word for it.
//
// The refusals -- Script, binary properties, an unknown name -- are
// regexp_unicode_property_refusals. Case folding under `u` and `i` is
// regexp_unicode_case_folding.

// The lone-value form and the name=value form name the same set, and every
// value has both an alias and a long name.
console.log(/\p{Lu}/u.test("A"), /\p{Lu}/u.test("a"));
console.log(/\p{General_Category=Lu}/u.test("A"), /\p{gc=Lu}/u.test("A"),
            /\p{Uppercase_Letter}/u.test("A"));

// A union is exactly the union of its members. `\p{L}` is Lu Ll Lt Lm Lo, and
// the five characters below are one of each: `A`, `a`, U+01C5 LATIN CAPITAL
// LETTER D WITH SMALL LETTER Z WITH CARON (Lt), U+02B0 MODIFIER LETTER SMALL H
// (Lm), and U+4E00 (Lo).
const letters = "Aa\u01c5\u02b0\u4e00";
console.log(/^\p{L}+$/u.test(letters));
console.log(/^[\p{Lu}\p{Ll}\p{Lt}\p{Lm}\p{Lo}]+$/u.test(letters));
console.log(/\p{L}/u.test("1"), /\p{L}/u.test(" "));

// `\p{Nd}` is the decimal digits of every script and not `[0-9]`. U+0663 is
// ARABIC-INDIC DIGIT THREE; U+2160 ROMAN NUMERAL ONE is Nl, so it is a Number
// without being a Decimal_Number.
console.log(/\p{Nd}/u.test("\u0663"), /\p{Nd}/u.test("x"), /\p{Nd}/u.test("\u2160"));
console.log(/\p{N}/u.test("\u2160"), /\p{Nl}/u.test("\u2160"));

// `\P` is the complement, over the code point ceiling: an astral character is
// "not a letter" and has to be in it.
console.log(/^\P{L}$/u.test("\u{1F600}"), /^\P{L}$/u.test("1"), /^\P{L}$/u.test("a"));
// An astral LETTER is still a letter. U+10400 DESERET CAPITAL LETTER LONG I is
// Lu and U+10428 is its small letter.
console.log(/^\p{L}$/u.test("\u{10400}"), /^\P{L}$/u.test("\u{10400}"));
console.log(/^\p{Lu}$/u.test("\u{10400}"), /^\p{Ll}$/u.test("\u{10428}"));
// A property and its complement partition the alphabet, so every character is
// in exactly one of them.
console.log(/^(?:\p{L}|\P{L})$/u.test("\u{1F600}"), /^\p{L}\P{L}$/u.test("a\u{1F600}"));

// Inside a class a property escape is a set the way `\d` is: it merges with the
// other members rather than becoming the endpoint of a range.
console.log(/^[\p{Nd}x-z]+$/u.test("x\u0663z"), /^[\p{Nd}x-z]+$/u.test("w"));
console.log(/^[^\p{L}]+$/u.test("12 "), /^[^\p{L}]+$/u.test("1a"));

// The three binary properties that follow from the same table and need no
// second one. U+0378 is unassigned in the UCD these tables were generated
// from, which is what makes it Cn and not Assigned.
console.log(/\p{ASCII}/u.test("A"), /\p{ASCII}/u.test("\u00e9"));
console.log(/\p{Cn}/u.test("\u0378"), /\p{Assigned}/u.test("\u0378"));
console.log(/\p{Assigned}/u.test("A"), /^\p{Any}$/u.test("\u{10FFFF}"));

// And the form a program actually writes: a global match over mixed text.
// U+00E9 is what makes the first run five letters long, and U+4E16 U+754C is
// the second run.
const text = "42 h\u00e9llo, \u4e16\u754c!";
console.log(text.match(/\p{L}+/gu).length, text.match(/\p{L}+/gu)[0].length);
console.log(text.replace(/\p{L}/gu, "-"));
