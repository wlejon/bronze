// 22.2.2.9 Canonicalize has two tables, and `u` together with `i` is which one
// gets read.
//
// With `i` alone, step 3 uppercases and step 4 keeps a non-ASCII character
// whose uppercase is ASCII. With `u` as well, step 1 applies SIMPLE CASE
// FOLDING -- the C and S mappings of CaseFolding.txt, and deliberately not the
// Turkic T ones -- which comes from the UCD by way of
// tools/gen_unicode_tables.py and covers every code point rather than the
// blocks bronze can state a rule for.
//
// The two tables agree about most of what they both cover, which is why the
// disagreements below are the whole point of carrying a second one. U+017F
// LATIN SMALL LETTER LONG S is the plainest: its uppercase is ASCII `S`, so
// step 4 leaves it alone and `/<long s>/i` does not match "s"; its folding is
// `s` with no such guard, so `/<long s>/ui` does. U+212A KELVIN SIGN is the
// same disagreement in a block the uppercase table has no rule for at all, so
// there `i` alone is a named error rather than a wrong answer.
//
// U+1E9E and U+00DF are the pair that decides whether the fold table was
// derived correctly. U+1E9E folds FULLY to "ss" and SIMPLY to U+00DF, and a
// table built on "a multi-character full folding means no simple folding"
// would have missed the second mapping and answered no here.
//
// 22.2.2.7.1 step 3 asserts WordCharacters gains members only when both flags
// are set, and this is the first mode bronze compiles where that is possible.
// It gains exactly two, and they are the same two -- which is why the last
// block below asks about them with `u` and without it.

function message(f) {
  try {
    f();
    return "no error";
  } catch (e) {
    return e instanceof SyntaxError ? e.message : "wrong error kind";
  }
}

function names(f, text) {
  return message(f).indexOf(text) >= 0;
}

// Where the two tables agree, the answer does not depend on which is read:
// Greek and Cyrillic are a plain offset in both.
console.log(/\u03a9/i.test("\u03c9"), /\u03a9/ui.test("\u03c9"));
console.log(/\u0410/i.test("\u0430"), /\u0410/ui.test("\u0430"));
// And a class range reaches its members through the reverse direction of
// whichever table is in play, so the small-letter range below finds two Greek
// capitals either way.
console.log(/^[\u03b1-\u03c9]+$/i.test("\u0393\u0394"),
            /^[\u03b1-\u03c9]+$/ui.test("\u0393\u0394"));

// U+017F, where they disagree.
console.log(/\u017f/i.test("s"), /\u017f/ui.test("s"));
console.log(/s/ui.test("\u017f"), /s/i.test("\u017f"));

// U+212A, the same disagreement one block further out, where `i` alone cannot
// be asked at all.
console.log(/\u212a/ui.test("k"), /k/ui.test("\u212a"));
console.log(names(function () { return new RegExp("\\u212a", "i"); }, "U+212A"),
            names(function () { return new RegExp("\\u212a", "ui"); }, "U+212A"));

// U+1E9E and U+00DF: one fold class, and a full folding that grows.
console.log(/\u1e9e/ui.test("\u00df"), /\u00df/ui.test("\u1e9e"));
console.log(/\u1e9e/ui.test("ss"), /\u00df/ui.test("SS"));
console.log(names(function () { return new RegExp("\\u1e9e", "i"); }, "U+1E9E"));

// The Turkic mappings are status T and ECMA-262 uses the non-Turkic table, so
// the dotted and dotless capitals fold to nothing while `I` and `i` fold
// together by the ordinary common mapping.
console.log(/\u0130/ui.test("I"), /\u0131/ui.test("i"), /I/ui.test("i"));

// Above the BMP, which `i` alone can never reach: U+10400 DESERET CAPITAL
// LETTER LONG I folds to U+10428, and a fold applied per code UNIT would leave
// both surrogate halves alone and answer no.
console.log(/\u{10400}/ui.test("\u{10428}"), /\u{10428}/ui.test("\u{10400}"));
console.log(/\u{10400}/u.test("\u{10428}"));
console.log(/[\u{10400}-\u{10427}]/ui.test("\u{10428}"));

// A backreference compares INPUT against INPUT, so it folds on its own -- per
// character, or an astral pair compares as two uncased surrogates.
console.log(/(\u{10400})\1/ui.test("\u{10400}\u{10428}"),
            /(\u{10400})\1/u.test("\u{10400}\u{10428}"));

// A property escape is a set, so `i` reaches it the way it reaches any class:
// the SET must hold a member canonicalizing to what the input canonicalizes
// to, which is not the same as testing the input's canonicalization.
console.log(/^\p{Lu}$/ui.test("a"), /^\p{Lu}$/u.test("a"));

// 22.2.2.7.1 step 3's non-empty case, and the same two characters refusing to
// be word characters in every other mode.
console.log(/\w/ui.test("\u017f"), /\w/ui.test("\u212a"));
console.log(/\w/i.test("\u017f"), /\w/u.test("\u017f"), /\w/.test("\u212a"));
// `\W` is the complement of the same set, and its ceiling is the code point
// one, so an astral character is in it.
console.log(/\W/ui.test("\u017f"), /^\W$/ui.test("\u{1F600}"));
// `\b` reads the same set, so two extra members move boundaries in patterns
// that never mention `\w` at all.
console.log(/a\B/ui.test("a\u017f"), /a\b/i.test("a\u017f"));
