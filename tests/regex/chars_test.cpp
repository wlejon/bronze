// The character data the pattern language reads: the case tables of 22.2.2.9,
// the WordCharacters set of 22.2.2.7.1, and the General_Category and simple
// case folding tables `tools/gen_unicode_tables.py` generates from the UCD.
//
// Split from `regex_test.cpp` along the seam the module itself has. That file
// asks what the grammar accepts and what the matcher does with it; this one
// asks what a CHARACTER is -- which two characters are the same under `i`,
// which set a class escape stands for, which category a code point is in. The
// two meet only through `chars.h`, and the questions have different failure
// modes: a grammar bug is a pattern that reads wrong, and a table bug is one
// code point in a million answering wrong and nothing else.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "pattern_fixture.h"
#include "regex/chars.h"
#include "regex/regex.h"
#include "regex/unicode.h"

using namespace bronze;
using namespace bronze::regex_fixture;

TEST_CASE("case-insensitive matching over ASCII and Latin-1") {
    CHECK(firstMatch(*compileOk("abc", "i"), "xxABCxx") == "ABC");
    CHECK(firstMatch(*compileOk("[a-z]+", "i"), "  ABC  ") == "ABC");
    // U+00DF's uppercase is two units, so 22.2.2.9 leaves it alone.
    std::string error;
    regex::Units input;
    input.push_back(0x00DF);
    auto pattern = compileOk("\\xDF", "i");
    regex::MatchResult result;
    CHECK(regex::search(*pattern, input, 0, result, error) == regex::ExecStatus::Match);
    // `/[µ]/i` matches U+039C, whose canonicalization U+00B5 also has.
    auto micro = compileOk("[\\xB5]", "i");
    regex::Units greek;
    greek.push_back(0x039C);
    CHECK(regex::search(*micro, greek, 0, result, error) == regex::ExecStatus::Match);
}

TEST_CASE("the case table above Latin-1, rule by rule") {
    // Greek is +32 from U+0391 Α to U+03A9 Ω, with U+03C2 FINAL SIGMA at -31
    // because it shares the capital U+03A3 with U+03C3.
    CHECK(matches(*compileOk("\\u03A9", "i"), unitsOf({0x03C9})));
    CHECK(matches(*compileOk("\\u03c3", "i"), unitsOf({0x03A3})));
    CHECK(matches(*compileOk("\\u03c2", "i"), unitsOf({0x03A3})));
    // The glyph-variant symbols uppercase to the ordinary capital of the
    // letter they vary, which is no offset at all.
    CHECK(matches(*compileOk("\\u03d0", "i"), unitsOf({0x0392})));
    CHECK(matches(*compileOk("\\u03f0", "i"), unitsOf({0x039A})));
    CHECK(matches(*compileOk("\\u03d1", "i"), unitsOf({0x0398})));
    // U+0390's uppercase is three code units, so step 3 leaves it alone and it
    // matches nothing but itself.
    CHECK(matches(*compileOk("\\u0390", "i"), unitsOf({0x0390})));
    CHECK_FALSE(matches(*compileOk("\\u0390", "i"), unitsOf({0x0399})));
    // Cyrillic is +32 for the alphabet at U+0410 and +80 for the block at
    // U+0400, whose small letters were encoded a row further down.
    CHECK(matches(*compileOk("\\u0410", "i"), unitsOf({0x0430})));
    CHECK(matches(*compileOk("\\u0451", "i"), unitsOf({0x0401})));
    // U+04CF SMALL PALOCHKA was added long after its capital U+04C0 and is the
    // one member of the block not adjacent to its pair.
    CHECK(matches(*compileOk("\\u04cf", "i"), unitsOf({0x04C0})));
    // Latin Extended-A is capital/small pairs, and these four are not: U+0131
    // and U+017F uppercase to ASCII (step 4 keeps them), U+0149's uppercase is
    // two units, and U+0130 is itself a capital.
    CHECK(matches(*compileOk("\\u0161", "i"), unitsOf({0x0160})));
    CHECK_FALSE(matches(*compileOk("\\u0131", "i"), unitsOf({0x0049})));
    CHECK_FALSE(matches(*compileOk("\\u017f", "i"), unitsOf({0x0053})));
    CHECK(matches(*compileOk("\\u0149", "i"), unitsOf({0x0149})));
    CHECK_FALSE(matches(*compileOk("\\u0131", "i"), unitsOf({0x0130})));
    // Armenian is +48.
    CHECK(matches(*compileOk("\\u0561", "i"), unitsOf({0x0531})));
}

TEST_CASE("a range folds through the reverse direction of the table") {
    // 22.2.2.7.1: the SET must hold a member canonicalizing to what the input
    // canonicalizes to. U+0393's own canonicalization is itself and is outside
    // [α-ω], so nothing but the reverse table finds U+03B3 for it.
    CHECK(matches(*compileOk("[\\u03b1-\\u03c9]", "i"), unitsOf({0x0393})));
    CHECK(matches(*compileOk("[\\u0391-\\u03a9]", "i"), unitsOf({0x03C2})));
    CHECK(matches(*compileOk("[\\u0430-\\u044f]", "i"), unitsOf({0x041F})));
    CHECK(matches(*compileOk("[\\u0561-\\u0586]", "i"), unitsOf({0x0556})));
    // A negated class inverts the ANSWER, after that membership test — not the
    // ranges before it.
    CHECK_FALSE(matches(*compileOk("[^\\u03b1-\\u03c9]", "i"), unitsOf({0x0393})));
    // 22.2.2.7.1's extra word characters are the units whose CANONICALIZATION
    // is already a word character, and step 3 asserts there are none without
    // `u`. U+017F and U+212A look like members and are not: 22.2.2.9 keeps a
    // non-ASCII unit whose uppercase is ASCII, so neither canonicalizes to
    // anything. The pair below is the equivalence — `\w` holds `ſ` exactly
    // when `/ſ/i` matches an `s` — and it is what the set and the comparison
    // used to answer differently.
    CHECK_FALSE(matches(*compileOk("\\w", "i"), unitsOf({0x017F})));
    CHECK_FALSE(matches(*compileOk("\\u017f", "i"), unitsOf({0x0073})));
    CHECK_FALSE(matches(*compileOk("\\w", "i"), unitsOf({0x212A})));
    CHECK_FALSE(matches(*compileOk("\\w"), unitsOf({0x017F})));
    CHECK_FALSE(matches(*compileOk("\\w"), unitsOf({0x212A})));
    // `\W` is the complement of the same set, so it follows rather than being
    // decided separately.
    CHECK(matches(*compileOk("\\W", "i"), unitsOf({0x017F})));
    CHECK(matches(*compileOk("\\W", "i"), unitsOf({0x212A})));
    CHECK_FALSE(matches(*compileOk("\\W", "i"), unitsOf({0x006B})));
    // The basic sixty-three are still there, and `i` still folds within them.
    CHECK(matches(*compileOk("\\w", "i"), unitsOf({0x004B})));
    CHECK(matches(*compileOk("\\w", "i"), unitsOf({0x006B})));
}

// The set the parser builds and the comparison the matcher runs are one
// answer (chars.h), and this is that sentence as a test: `\w` holds a
// character exactly when it is one of the basic sixty-three or `canonicalize`
// makes it one. Driven over the whole alphabet of each of the four modes,
// because the two agreed for all but two characters and it was those two that
// were wrong — first by being in the set without `u`, and then, had the
// derivation been abandoned, by being out of it with `u`.
//
// Under `u` the derivation in `chars.cpp` walks the fold table's sources
// rather than the code space, which is the same answer only if nothing outside
// that table folds. The walk below is over the code space, so it is the check
// on that shortcut and not a repetition of it.
TEST_CASE("wordRanges is derived from canonicalize and cannot disagree with it") {
    regex::RangeList basic;
    regex::addRange(basic, '0', '9');
    regex::addRange(basic, 'A', 'Z');
    regex::addRange(basic, '_', '_');
    regex::addRange(basic, 'a', 'z');
    regex::normalizeRanges(basic);

    for (bool unicode : {false, true}) {
        for (bool ignoreCase : {false, true}) {
            const regex::RangeList& words = regex::wordRanges(ignoreCase, unicode);
            size_t disagreements = 0;
            for (uint32_t c = 0; c <= regex::alphabetCeiling(unicode); ++c) {
                const bool derived =
                    regex::rangesContain(basic, c) ||
                    regex::rangesContain(basic, regex::canonicalize(c, ignoreCase, unicode));
                if (regex::rangesContain(words, c) != derived) ++disagreements;
            }
            CHECK(disagreements == 0);
        }
    }
    // 22.2.2.7.1 step 3: the extra set is EMPTY unless both [[Unicode]] and
    // [[IgnoreCase]] hold. Three of the four modes are the same sixty-three, in
    // the same four ranges.
    CHECK(regex::wordRanges(false, false).size() == 4);
    CHECK(regex::wordRanges(true, false).size() == 4);
    CHECK(regex::wordRanges(false, true).size() == 4);
    // The fourth holds exactly two more members, and they are the two the whole
    // argument has always been about: simple case folding sends U+017F to `s`
    // and U+212A to `k`, where the uppercase table leaves both alone.
    const regex::RangeList& folded = regex::wordRanges(true, true);
    CHECK(folded.size() == 6);
    CHECK(regex::rangesContain(folded, 0x017F));
    CHECK(regex::rangesContain(folded, 0x212A));
    CHECK_FALSE(regex::rangesContain(regex::wordRanges(true, false), 0x017F));
    CHECK_FALSE(regex::rangesContain(regex::wordRanges(true, false), 0x212A));
    // `\b` and `\w` read a code UNIT rather than decoding a character, which is
    // sound only while no member of the set is astral or is half of a surrogate
    // pair. That is a property of the fold table rather than of the matcher, so
    // it is checked here and asserted nowhere.
    for (const regex::Range& r : folded) {
        CHECK(r.hi <= 0xFFFF);
        const bool surrogate = r.lo >= 0xD800 && r.lo <= 0xDFFF;
        CHECK_FALSE(surrogate);
    }
}

// The refused-block query a class range asks. It is an interval overlap over a
// short sorted list, so the cost does not grow with the range — and it reports
// the LOWEST refused unit, which is why the list's order is checked at compile
// time.
TEST_CASE("the refused-case blocks are queried as intervals, not per unit") {
    uint32_t offender = 0;
    // A range that spells no refused unit but contains a whole block of them.
    CHECK(regex::firstUnknownCasedUnitInRange(0x00FF, 0x2000, offender));
    CHECK(offender == 0x0180);  // Latin Extended-B, the first block above U+00FF
    // The whole plane reports the first block of all.
    CHECK(regex::firstUnknownCasedUnitInRange(0x0000, 0xFFFF, offender));
    CHECK(offender == 0x0180);
    // An endpoint inside a block reports that endpoint, not the block's start.
    CHECK(regex::firstUnknownCasedUnitInRange(0x0200, 0x0210, offender));
    CHECK(offender == 0x0200);
    // Ranges that thread the holes: the blocks bronze DOES fold are untouched,
    // and each of these sits exactly between two refused blocks.
    CHECK_FALSE(regex::firstUnknownCasedUnitInRange(0x0391, 0x03A9, offender));
    CHECK_FALSE(regex::firstUnknownCasedUnitInRange(0x0430, 0x044F, offender));
    CHECK_FALSE(regex::firstUnknownCasedUnitInRange(0x0561, 0x0586, offender));
    CHECK_FALSE(regex::firstUnknownCasedUnitInRange('a', 'z', offender));
    // One unit either side of a block boundary, so an off-by-one shows up.
    CHECK_FALSE(regex::firstUnknownCasedUnitInRange(0x017F, 0x017F, offender));
    CHECK(regex::firstUnknownCasedUnitInRange(0x0180, 0x0180, offender));
    // The single-unit question is the same function, so the two cannot drift.
    for (uint32_t u = 0; u <= regex::kMaxUnit; ++u) {
        uint32_t ignored = 0;
        if (regex::isUnknownCasedUnit(u) != regex::firstUnknownCasedUnitInRange(u, u, ignored)) {
            FAIL("isUnknownCasedUnit disagrees with the range query at U+", u);
        }
    }
}

TEST_CASE("a case fold bronze has no table for is a named error, never a guess") {
    // U+1E9E LATIN CAPITAL LETTER SHARP S: Latin Extended Additional is one of
    // the blocks whose mappings bronze states no rule for.
    const std::string error = compileError("\\u1E9E", "i");
    CHECK(error.find("U+1E9E") != std::string::npos);
    CHECK(error.find("without the `u` flag") != std::string::npos);
    // Without `i` the same pattern is ordinary — and WITH `u` as well as `i` it
    // compiles, because that mode reads the generated fold table, which has no
    // holes for the refusal to be about.
    CHECK(compileOk("\\u1E9E") != nullptr);
    CHECK(compileOk("\\u1E9E", "ui") != nullptr);
    CHECK(compileOk("[\\u0000-\\uffff]", "ui") != nullptr);
    // Latin Extended-B and Georgian are refused for the same reason, and
    // U+037A is refused inside a block that otherwise folds.
    CHECK(compileError("\\u01C5", "i").find("U+01C5") != std::string::npos);
    CHECK(compileError("\\u10A0", "i").find("U+10A0") != std::string::npos);
    CHECK(compileError("\\u037A", "i").find("U+037A") != std::string::npos);
    // A class RANGE is refused for what it CONTAINS and not for what it
    // spells. Neither endpoint below is refused, and everything from U+0180 to
    // U+02FF between them is — so the endpoint-only check compiled this and
    // then answered plain containment for U+1E9E, silently skipping the fold.
    CHECK(compileError("[\\u00ff-\\u2000]", "i").find("U+0180") != std::string::npos);
    CHECK(compileError("[\\u0000-\\uffff]", "i").find("U+0180") != std::string::npos);
    // Without `i` there is no fold to be wrong about, so the same class is an
    // ordinary one.
    CHECK(compileOk("[\\u00ff-\\u2000]") != nullptr);
    CHECK(matches(*compileOk("[\\u00ff-\\u2000]"), unitsOf({0x1E9E})));
    // The blocks that DO fold are not refused — a refusal that outlived its
    // table would be a hard error for nothing.
    CHECK(compileOk("\\u03A9", "i") != nullptr);
    CHECK(compileOk("\\u0410", "i") != nullptr);
    CHECK(compileOk("\\u0161", "i") != nullptr);
    CHECK(compileOk("\\u0561", "i") != nullptr);
}

TEST_CASE("a property escape is a +UnicodeMode production, and refused without it") {
    // Reading `\p{L}` as the letter `p` is what Annex B would do, and it is the
    // silent wrong answer the refusal exists to prevent: a pattern written to
    // match letters would match the letter p.
    CHECK(compileError("\\p{L}").find("unicode property escapes") != std::string::npos);
    CHECK(compileError("\\P{L}").find("unicode property escapes") != std::string::npos);
    CHECK(compileError("\\p{L}").find("`u` flag") != std::string::npos);
}

TEST_CASE("the properties bronze does not carry are refused by name") {
    // Script is refused by NAME rather than through the general message, and
    // above all is never read as a General_Category lookup that happens to find
    // nothing. There is no honest source for it here at all.
    const std::string script = compileError("\\p{Script=Greek}", "u");
    CHECK(script.find("Script") != std::string::npos);
    CHECK(script.find("UAX #24") != std::string::npos);
    CHECK(compileError("\\p{sc=Greek}", "u").find("Script") != std::string::npos);
    CHECK(compileError("\\p{Script_Extensions=Greek}", "u").find("Script") != std::string::npos);
    CHECK(compileError("\\p{scx=Greek}", "u").find("Script") != std::string::npos);
    // A binary property bronze cannot derive. The message names it and says
    // what IS carried, because "no" on its own cannot be acted on.
    const std::string binary = compileError("\\p{Alphabetic}", "u");
    CHECK(binary.find("Alphabetic") != std::string::npos);
    CHECK(binary.find("General_Category") != std::string::npos);
    CHECK(compileError("\\p{White_Space}", "u").find("White_Space") != std::string::npos);
    CHECK(compileError("\\p{Emoji}", "u").find("Emoji") != std::string::npos);
    // A misspelling is a syntax error naming the offender, never a set that
    // silently matches nothing.
    CHECK(compileError("\\p{Lx}", "u").find("Lx") != std::string::npos);
    CHECK(compileError("\\p{lu}", "u").find("lu") != std::string::npos);
    CHECK(compileError("\\p{General_Category=Nope}", "u").find("Nope") != std::string::npos);
    CHECK(compileError("\\p{Bidi_Class=L}", "u").find("Bidi_Class") != std::string::npos);
    // And the syntax around it is checked too, rather than being read as text.
    CHECK(compileError("\\pL", "u").find("`{` was expected") != std::string::npos);
    CHECK(compileError("\\p{L", "u").find("`}` was expected") != std::string::npos);
    CHECK(compileError("\\p{}", "u").find("names no property") != std::string::npos);
}

TEST_CASE("General_Category, by alias, by long name, and as a union") {
    // The lone-value form and the name=value form are the same set.
    CHECK(matches(*compileOk("\\p{Lu}", "u"), unitsOf({'A'})));
    CHECK_FALSE(matches(*compileOk("\\p{Lu}", "u"), unitsOf({'a'})));
    CHECK(matches(*compileOk("\\p{General_Category=Lu}", "u"), unitsOf({'A'})));
    CHECK(matches(*compileOk("\\p{gc=Lu}", "u"), unitsOf({'A'})));
    CHECK(matches(*compileOk("\\p{Uppercase_Letter}", "u"), unitsOf({'A'})));
    // A union is exactly the union of its members, which is the whole reason
    // `unicode.cpp` spells the unions as member lists and not as ranges.
    for (uint32_t c : {uint32_t('A'), uint32_t('a'), 0x01C5u, 0x02B0u, 0x4E00u}) {
        CHECK(matches(*compileOk("\\p{L}", "u"), unitsOf({c})));
    }
    CHECK_FALSE(matches(*compileOk("\\p{L}", "u"), unitsOf({'1'})));
    CHECK(matches(*compileOk("\\p{Nd}", "u"), unitsOf({0x0663})));  // ARABIC-INDIC DIGIT THREE
    CHECK_FALSE(matches(*compileOk("\\p{Nd}", "u"), unitsOf({'x'})));
    CHECK(matches(*compileOk("\\p{N}", "u"), unitsOf({0x2160})));  // ROMAN NUMERAL ONE, Nl
    CHECK_FALSE(matches(*compileOk("\\p{Nd}", "u"), unitsOf({0x2160})));
    // Cn is a General_Category value like any other, so unassigned code points
    // are nameable — and `Assigned` is its complement rather than a table.
    CHECK(matches(*compileOk("\\p{Cn}", "u"), textOf({0x0378})));
    CHECK_FALSE(matches(*compileOk("\\p{Assigned}", "u"), textOf({0x0378})));
    CHECK(matches(*compileOk("\\p{Assigned}", "u"), unitsOf({'A'})));
    CHECK(matches(*compileOk("\\p{ASCII}", "u"), unitsOf({'A'})));
    CHECK_FALSE(matches(*compileOk("\\p{ASCII}", "u"), unitsOf({0x00E9})));
    CHECK(matches(*compileOk("\\p{Any}", "u"), textOf({0x10FFFF})));
}

// `\p{L}` must be the same set as `Lu | Ll | Lt | Lm | Lo`, and the sets must
// be the same answer the table itself gives, over every code point. Walking it
// is what turns "the unions are derived from their members" from a claim about
// how `unicode.cpp` is written into a property of what it computes.
TEST_CASE("the property table agrees with itself over the whole code space") {
    regex::RangeList letters;
    std::string error;
    REQUIRE(regex::unicodePropertySet("", "L", letters, error));
    regex::RangeList members;
    for (const char* alias : {"Lu", "Ll", "Lt", "Lm", "Lo"}) {
        regex::RangeList one;
        REQUIRE(regex::unicodePropertySet("", alias, one, error));
        members.insert(members.end(), one.begin(), one.end());
    }
    regex::normalizeRanges(members);
    REQUIRE(letters.size() == members.size());
    for (size_t i = 0; i < letters.size(); ++i) {
        CHECK(letters[i].lo == members[i].lo);
        CHECK(letters[i].hi == members[i].hi);
    }

    // And every code point's own category puts it in exactly the sets that
    // name it. Driven over all 1114112, since a binary search that is off by
    // one at a run boundary is invisible in any smaller sample.
    regex::RangeList upper;
    REQUIRE(regex::unicodePropertySet("General_Category", "Lu", upper, error));
    regex::RangeList unassigned;
    REQUIRE(regex::unicodePropertySet("", "Cn", unassigned, error));
    size_t disagreements = 0;
    for (uint32_t c = 0; c <= regex::kMaxCodePoint; ++c) {
        const std::string_view category = regex::generalCategoryOf(c);
        if (regex::rangesContain(upper, c) != (category == "Lu")) ++disagreements;
        if (regex::rangesContain(unassigned, c) != (category == "Cn")) ++disagreements;
        if (regex::rangesContain(letters, c) != (category[0] == 'L')) ++disagreements;
    }
    CHECK(disagreements == 0);
}

TEST_CASE("`\\P` complements over the code point ceiling") {
    // The complement of a property is "everything else" and everything else
    // reaches U+10FFFF. A `\P{L}` that stopped at U+FFFF would exclude every
    // astral character from a set whose whole meaning is "not a letter".
    CHECK(matches(*compileOk("\\P{L}", "u"), unitsOf({'1'})));
    CHECK_FALSE(matches(*compileOk("\\P{L}", "u"), unitsOf({'a'})));
    CHECK(matches(*compileOk("^\\P{L}$", "u"), textOf({0x1F600})));
    // An astral LETTER is still a letter, so the complement must not hold it.
    // U+10400 DESERET CAPITAL LETTER LONG I is Lu.
    CHECK(matches(*compileOk("^\\p{L}$", "u"), textOf({0x10400})));
    CHECK_FALSE(matches(*compileOk("^\\P{L}$", "u"), textOf({0x10400})));
    CHECK(matches(*compileOk("^\\p{Lu}$", "u"), textOf({0x10400})));
    // Inside a class, and combined with a range — a property escape is a set
    // like `\d` is, so it merges rather than becoming a range endpoint.
    CHECK(matches(*compileOk("^[\\p{Nd}x-z]+$", "u"), unitsOf({'x', 0x0663, 'z'})));
    CHECK_FALSE(matches(*compileOk("^[\\p{Nd}x-z]+$", "u"), unitsOf({'w'})));
    CHECK(matches(*compileOk("^[^\\p{L}]$", "u"), unitsOf({'1'})));
    CHECK_FALSE(matches(*compileOk("^[^\\p{L}]$", "u"), textOf({0x10400})));
    CHECK(compileError("[a-\\p{L}]", "u").find("cannot be the end of a range") !=
          std::string::npos);
}

// Simple case folding, at the entries that decide whether the table was
// DERIVED correctly rather than merely present. Each one below is a
// CaseFolding.txt status whose consequence differs from the others'.
TEST_CASE("simple case folding is the C and S mappings, and not the T ones") {
    // Status C: a single-character folding shared by the simple and the full
    // form, which is most of the table.
    CHECK(regex::simpleCaseFold('A') == 'a');
    CHECK(regex::simpleCaseFold('a') == 'a');
    CHECK(regex::simpleCaseFold(0x03A3) == 0x03C3);  // capital sigma
    CHECK(regex::simpleCaseFold(0x03C2) == 0x03C3);  // FINAL sigma, same class
    CHECK(regex::simpleCaseFold(0x00B5) == 0x03BC);  // micro sign to small mu
    CHECK(regex::simpleCaseFold(0x017F) == 0x0073);  // long s to `s`
    CHECK(regex::simpleCaseFold(0x212A) == 0x006B);  // kelvin sign to `k`
    // Status F with no S: the full folding grows the string, so scf leaves the
    // character alone. U+00DF folds fully to "ss" and simply to itself.
    CHECK(regex::simpleCaseFold(0x00DF) == 0x00DF);
    CHECK(regex::simpleCaseFold(0x0149) == 0x0149);
    CHECK(regex::simpleCaseFold(0xFB00) == 0xFB00);
    // Status F WITH an S: this is the pair a "multi-character full folding
    // means no simple folding" shortcut gets wrong. U+1E9E folds fully to "ss"
    // and simply to U+00DF, so it and the sharp s are one class.
    CHECK(regex::simpleCaseFold(0x1E9E) == 0x00DF);
    CHECK(regex::simpleCaseFold(0x1F88) == 0x1F80);
    CHECK(regex::simpleCaseFold(0x1FFC) == 0x1FF3);
    // Status T: the Turkic mappings, which ECMA-262 does not use. `I` and `i`
    // fold together; the dotted and dotless capitals fold to neither and to
    // nothing.
    CHECK(regex::simpleCaseFold(0x0049) == 0x0069);
    CHECK(regex::simpleCaseFold(0x0130) == 0x0130);
    CHECK(regex::simpleCaseFold(0x0131) == 0x0131);
    // Above the BMP, which is the half of the table `i` alone can never reach.
    CHECK(regex::simpleCaseFold(0x10400) == 0x10428);  // DESERET CAPITAL LONG I
    CHECK(regex::simpleCaseFold(0x10428) == 0x10428);
    // A code point with no case at all, on each side of every table boundary
    // the binary search can be off by one at.
    CHECK(regex::simpleCaseFold(0) == 0);
    CHECK(regex::simpleCaseFold(0x0040) == 0x0040);
    CHECK(regex::simpleCaseFold(0x005B) == 0x005B);
    CHECK(regex::simpleCaseFold(0x4E00) == 0x4E00);
    CHECK(regex::simpleCaseFold(0xD800) == 0xD800);
    CHECK(regex::simpleCaseFold(0x10FFFF) == 0x10FFFF);
    // Idempotent, which 22.2.2.9's use of it assumes: the pattern's character
    // and the input's are canonicalized independently and then compared.
    for (uint32_t source : regex::simpleCaseFoldSources()) {
        const uint32_t folded = regex::simpleCaseFold(source);
        CHECK(regex::simpleCaseFold(folded) == folded);
    }
    // The reverse direction holds every source and nothing that folds to
    // itself, which is what 22.2.2.7.1 asks the SET about.
    const std::vector<uint32_t>& toS = regex::simpleCaseFoldCandidates('s');
    CHECK(std::find(toS.begin(), toS.end(), 'S') != toS.end());
    CHECK(std::find(toS.begin(), toS.end(), 0x017F) != toS.end());
    CHECK(std::find(toS.begin(), toS.end(), 's') == toS.end());
    CHECK(regex::simpleCaseFoldCandidates(0x00DF).size() == 1);
    CHECK(regex::simpleCaseFoldCandidates(0x00DF)[0] == 0x1E9E);
    CHECK(regex::simpleCaseFoldCandidates(0x4E00).empty());
}

TEST_CASE("`u` with `i` canonicalizes by folding, which is a different table") {
    // Where the two tables agree, the answer does not depend on which is read:
    // Greek and Cyrillic are a plain offset in both.
    CHECK(matches(*compileOk("\\u03a9", "i"), unitsOf({0x03C9})));
    CHECK(matches(*compileOk("\\u03a9", "ui"), unitsOf({0x03C9})));
    CHECK(matches(*compileOk("\\u0410", "i"), unitsOf({0x0430})));
    CHECK(matches(*compileOk("\\u0410", "ui"), unitsOf({0x0430})));
    CHECK(matches(*compileOk("[\\u03b1-\\u03c9]", "ui"), unitsOf({0x0393})));

    // And where they disagree is the entire reason for the second table.
    // U+017F: `toUppercase` gives ASCII `S`, and 22.2.2.9 step 4 keeps the
    // character unchanged for exactly that reason — so `/ſ/i` does not match
    // "s". Simple case folding has no such guard, so `/ſ/ui` does.
    CHECK_FALSE(matches(*compileOk("\\u017f", "i"), unitsOf({'s'})));
    CHECK(matches(*compileOk("\\u017f", "ui"), unitsOf({'s'})));
    CHECK(matches(*compileOk("s", "ui"), unitsOf({0x017F})));
    // U+212A KELVIN SIGN is the same disagreement one block further out, where
    // `i` alone cannot even be asked: Letterlike Symbols is a block the
    // uppercase table has no rule for, so the pattern is a named error there
    // and an ordinary match under `u`.
    CHECK(compileError("\\u212a", "i").find("U+212A") != std::string::npos);
    CHECK(matches(*compileOk("\\u212a", "ui"), unitsOf({'k'})));
    // U+1E9E and U+00DF are one fold class and two different uppercase
    // answers, which is the status-S entry the table would miss if the fold
    // had been derived from the full folding's length alone.
    CHECK(matches(*compileOk("\\u1e9e", "ui"), unitsOf({0x00DF})));
    CHECK(matches(*compileOk("\\u00df", "ui"), unitsOf({0x1E9E})));
    // The Turkic pair stays apart, because scf uses neither T mapping.
    CHECK_FALSE(matches(*compileOk("\\u0130", "ui"), unitsOf({'I'})));
    CHECK_FALSE(matches(*compileOk("\\u0131", "ui"), unitsOf({'i'})));
    CHECK(matches(*compileOk("I", "ui"), unitsOf({'i'})));

    // Above the BMP, where `i` alone has nothing to say at all: U+10400 folds
    // to U+10428, and a fold applied per code UNIT would leave both surrogate
    // halves alone and answer no.
    CHECK(matches(*compileOk("^.$", "ui"), textOf({0x10400})));
    CHECK(matches(*compileUnitsOk(textOf({0x10400}), "ui"), textOf({0x10428})));
    CHECK(matches(*compileUnitsOk(textOf({0x10428}), "ui"), textOf({0x10400})));
    CHECK_FALSE(matches(*compileUnitsOk(textOf({0x10400}), "u"), textOf({0x10428})));
    // A class range over the astral capitals finds a small letter through the
    // reverse direction of the fold, the same way `[α-ω]` finds `Γ`.
    CHECK(matches(*compileOk("[\\u{10400}-\\u{10427}]", "ui"), textOf({0x10428})));
    // A property escape is a set, so `i` reaches it the same way: `\p{Lu}`
    // matches a lowercase letter under `ui` because some member of the set
    // canonicalizes to what the input canonicalizes to.
    CHECK(matches(*compileOk("\\p{Lu}", "ui"), unitsOf({'a'})));
    CHECK_FALSE(matches(*compileOk("\\p{Lu}", "u"), unitsOf({'a'})));

    // A backreference compares INPUT against INPUT, so it does its own fold —
    // and it has to do it per character, or an astral pair compares as two
    // uncased surrogates and answers no.
    CHECK(matches(*compileUnitsOk(textOf({'(', 0x10400, ')', '\\', '1'}), "ui"),
                  textOf({0x10400, 0x10428})));
    CHECK_FALSE(matches(*compileUnitsOk(textOf({'(', 0x10400, ')', '\\', '1'}), "u"),
                        textOf({0x10400, 0x10428})));
    CHECK(matches(*compileOk("(\\u017f)\\1", "ui"), unitsOf({0x017F, 's'})));
    // Without `u` the same comparison is refused rather than guessed at, since
    // the uppercase table has holes and both sides are input.
    CHECK(matches(*compileOk("(a)\\1", "i"), unitsOf({'a', 'A'})));
}

TEST_CASE("WordCharacters grows by exactly two members under `u` and `i`") {
    // 22.2.2.7.1 step 3's non-empty case, and the same characters that must NOT
    // be members without `u`. Both answers come from `canonicalize`, so neither
    // can be fixed without the other moving.
    CHECK(matches(*compileOk("\\w", "ui"), unitsOf({0x017F})));
    CHECK(matches(*compileOk("\\w", "ui"), unitsOf({0x212A})));
    CHECK_FALSE(matches(*compileOk("\\w", "i"), unitsOf({0x017F})));
    CHECK_FALSE(matches(*compileOk("\\w", "u"), unitsOf({0x017F})));
    CHECK_FALSE(matches(*compileOk("\\w", ""), unitsOf({0x017F})));
    // `\W` is the complement of the same set, so it follows rather than being
    // decided separately — and its ceiling is the code point one.
    CHECK_FALSE(matches(*compileOk("\\W", "ui"), unitsOf({0x017F})));
    CHECK(matches(*compileOk("\\W", "ui"), unitsOf({0x00E9})));
    CHECK(matches(*compileOk("^\\W$", "ui"), textOf({0x1F600})));
    // `\b` consults the same set, so two extra members move word boundaries in
    // patterns that never mention `\w`.
    CHECK(matches(*compileOk("a\\B", "ui"), unitsOf({'a', 0x017F})));
    CHECK(matches(*compileOk("a\\b", "i"), unitsOf({'a', 0x017F})));
    CHECK(matches(*compileOk("a\\b", "u"), unitsOf({'a', 0x017F})));
}
