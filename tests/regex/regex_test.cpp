// The pattern grammar and the matcher, driven directly. Nothing here needs a
// heap, a Value or a compiled program, which is the point of the module being
// its own static library: every question below is decided by ECMA-262 22.2
// alone.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "regex/chars.h"
#include "regex/regex.h"

using namespace bronze;

namespace {

regex::Units u(std::string_view ascii) {
    regex::Units out;
    for (char c : ascii) out.push_back(static_cast<uint16_t>(static_cast<unsigned char>(c)));
    return out;
}

regex::Flags flagsOf(std::string_view text) {
    regex::Flags flags;
    std::string error;
    REQUIRE(regex::parseFlags(text, flags, error));
    return flags;
}

regex::PatternPtr compileOk(std::string_view source, std::string_view flagText = "") {
    std::string error;
    auto pattern = regex::compile(u(source), flagsOf(flagText), error);
    REQUIRE_MESSAGE(pattern != nullptr, error);
    return pattern;
}

std::string compileError(std::string_view source, std::string_view flagText = "") {
    std::string error;
    auto pattern = regex::compile(u(source), flagsOf(flagText), error);
    CHECK(pattern == nullptr);
    return error;
}

// The whole match's text, or "<none>". Written as a string so a failing case
// reads as the thing the pattern was supposed to find.
std::string firstMatch(const regex::Pattern& pattern, std::string_view input, size_t from = 0) {
    regex::MatchResult result;
    std::string error;
    const regex::Units units = u(input);
    if (regex::search(pattern, units, from, result, error) != regex::ExecStatus::Match) {
        return error.empty() ? "<none>" : "<error: " + error + ">";
    }
    std::string out;
    for (int64_t i = result.start(); i < result.end(); ++i) out.push_back(input[static_cast<size_t>(i)]);
    return out;
}

// A string spelled by code UNIT, for the case table: `u()` above is ASCII
// only, and a fold question is never about a character that fits in it. It is
// also how a LONE surrogate is written, which no code-point spelling can say.
regex::Units unitsOf(std::initializer_list<uint32_t> codes) {
    regex::Units out;
    for (uint32_t c : codes) out.push_back(static_cast<char16_t>(c));
    return out;
}

// A string spelled by CODE POINT, with anything above 0xFFFF encoded as the
// surrogate pair a JavaScript string holds for it. Used for both subjects and
// pattern text, since under `u` a pattern may spell an astral character
// directly.
regex::Units textOf(std::initializer_list<uint32_t> codes) {
    regex::Units out;
    for (uint32_t c : codes) {
        if (c > 0xFFFF) {
            const uint32_t v = c - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

regex::PatternPtr compileUnitsOk(const regex::Units& source, std::string_view flagText) {
    std::string error;
    auto pattern = regex::compile(source, flagsOf(flagText), error);
    REQUIRE_MESSAGE(pattern != nullptr, error);
    return pattern;
}

// The half-open extent of the first match, as "start..end". A `u` question is
// almost always about how far the index moved, which the matched TEXT cannot
// show and this can.
std::string extent(const regex::Pattern& pattern, const regex::Units& input, size_t from = 0) {
    regex::MatchResult result;
    std::string error;
    if (regex::search(pattern, input, from, result, error) != regex::ExecStatus::Match) {
        return error.empty() ? "<none>" : "<error: " + error + ">";
    }
    return std::to_string(result.start()) + ".." + std::to_string(result.end());
}

bool matches(const regex::Pattern& pattern, const regex::Units& input) {
    regex::MatchResult result;
    std::string error;
    return regex::search(pattern, input, 0, result, error) == regex::ExecStatus::Match;
}

std::string capture(const regex::Pattern& pattern, std::string_view input, uint32_t group) {
    regex::MatchResult result;
    std::string error;
    const regex::Units units = u(input);
    if (regex::search(pattern, units, 0, result, error) != regex::ExecStatus::Match) return "<none>";
    const int64_t start = result.captures[group * 2];
    const int64_t end = result.captures[group * 2 + 1];
    if (start == regex::MatchResult::kUnset) return "<undefined>";
    std::string out;
    for (int64_t i = start; i < end; ++i) out.push_back(input[static_cast<size_t>(i)]);
    return out;
}

}  // namespace

TEST_CASE("flags parse in the order 22.2.6.5 pins") {
    regex::Flags flags = flagsOf("yig");
    CHECK(flags.global);
    CHECK(flags.ignoreCase);
    CHECK(flags.sticky);
    CHECK(flags.text() == "giy");
}

TEST_CASE("unimplemented flags are refused by name") {
    regex::Flags flags;
    std::string error;
    CHECK_FALSE(regex::parseFlags("d", flags, error));
    CHECK(error.find("match indices") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("v", flags, error));
    CHECK(error.find("`v` flag") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("gg", flags, error));
    CHECK(error.find("more than once") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("q", flags, error));
    // `u` alone is a mode; `u` with `i` is a COMBINATION bronze refuses, and it
    // is refused where they meet rather than at either letter — so each of them
    // still parses on its own.
    CHECK(regex::parseFlags("u", flags, error));
    CHECK(flags.unicode);
    CHECK(regex::parseFlags("i", flags, error));
    CHECK_FALSE(regex::parseFlags("ui", flags, error));
    CHECK(error.find("`u` and `i` flags together") != std::string::npos);
    CHECK(error.find("simple case folding") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("iu", flags, error));
    CHECK(error.find("`u` and `i` flags together") != std::string::npos);
    // 22.2.6.5's order puts `u` between `s` and `y`.
    CHECK(regex::parseFlags("yug", flags, error));
    CHECK(flags.text() == "guy");
}

TEST_CASE("literals, alternation and quantifiers") {
    CHECK(firstMatch(*compileOk("abc"), "xxabcxx") == "abc");
    CHECK(firstMatch(*compileOk("a|bb|ccc"), "xxcccxx") == "ccc");
    CHECK(firstMatch(*compileOk("a+"), "baaab") == "aaa");
    CHECK(firstMatch(*compileOk("a+?"), "baaab") == "a");
    CHECK(firstMatch(*compileOk("a{2,3}"), "aaaaa") == "aaa");
    CHECK(firstMatch(*compileOk("a{2,}"), "aaaaa") == "aaaaa");
    CHECK(firstMatch(*compileOk("a{2}"), "aaaaa") == "aa");
    CHECK(firstMatch(*compileOk("colou?r"), "color") == "color");
    // The alternation is ordered, not longest-match: `a` wins at index 0.
    CHECK(firstMatch(*compileOk("a|ab"), "ab") == "a");
    // IdentityEscape (22.2.1): `\` before a SyntaxCharacter is that character,
    // which is the only way to write a literal `$`. Only a LETTER OR DIGIT is
    // refused, since that is where Annex B and the strict grammar diverge.
    CHECK(firstMatch(*compileOk("\\$\\d+"), "cost: $42") == "$42");
    CHECK(firstMatch(*compileOk("a\\-b"), "xa-by") == "a-b");
}

TEST_CASE("an invalid brace is an ordinary character (Annex B)") {
    CHECK(firstMatch(*compileOk("a{"), "xa{y") == "a{");
    CHECK(firstMatch(*compileOk("{,3}"), "{,3}") == "{,3}");
}

TEST_CASE("character classes and ranges") {
    CHECK(firstMatch(*compileOk("[a-c]+"), "xxabcyy") == "abc");
    CHECK(firstMatch(*compileOk("[^a-c]+"), "abcxyzabc") == "xyz");
    CHECK(firstMatch(*compileOk("[\\d]+"), "ab123cd") == "123");
    CHECK(firstMatch(*compileOk("[\\w-]+"), "  a-b_1  ") == "a-b_1");
    CHECK(firstMatch(*compileOk("\\s+"), "ab \t cd") == " \t ");
    CHECK(firstMatch(*compileOk("[\\]]"), "x]y") == "]");
    // `[]` is an EMPTY class in ECMA-262 (22.2.1's ClassRanges may be empty),
    // so it matches nothing at all and `[^]` matches anything — the exact
    // opposite of the POSIX reading, where `[]]` is a class holding `]`.
    CHECK(firstMatch(*compileOk("[]]"), "x]y") == "<none>");
    CHECK(firstMatch(*compileOk("[^]"), "x") == "x");
    // A `-` immediately before `]` is a member, not a broken range.
    CHECK(firstMatch(*compileOk("[a-]+"), "xa-ay") == "a-a");
}

TEST_CASE("dot, anchors and the flags that change them") {
    CHECK(firstMatch(*compileOk("a.c"), "a\nc") == "<none>");
    CHECK(firstMatch(*compileOk("a.c", "s"), "a\nc") == "a\nc");
    CHECK(firstMatch(*compileOk("^b"), "a\nb") == "<none>");
    CHECK(firstMatch(*compileOk("^b", "m"), "a\nb") == "b");
    CHECK(firstMatch(*compileOk("a$"), "a\nb") == "<none>");
    CHECK(firstMatch(*compileOk("a$", "m"), "a\nb") == "a");
    CHECK(firstMatch(*compileOk("\\bworld\\b"), "hello world!") == "world");
    CHECK(firstMatch(*compileOk("\\Bell"), "hello") == "ell");
}

TEST_CASE("groups, backreferences and named groups") {
    CHECK(capture(*compileOk("(a)(b)?"), "a", 2) == "<undefined>");
    CHECK(firstMatch(*compileOk("(ab)\\1"), "xabab") == "abab");
    CHECK(firstMatch(*compileOk("(?:ab)+"), "ababab") == "ababab");
    CHECK(capture(*compileOk("(?<year>\\d{4})-(?<month>\\d{2})"), "on 2026-08-11", 1) == "2026");
    CHECK(capture(*compileOk("(?<year>\\d{4})-(?<month>\\d{2})"), "on 2026-08-11", 2) == "08");
    auto named = compileOk("(?<x>a)\\k<x>");
    CHECK(regex::hasNamedGroups(*named));
    CHECK(regex::groupName(*named, 1) == "x");
    CHECK(firstMatch(*named, "zaaz") == "aa");
    // A group that never participated backreferences as the empty string.
    CHECK(firstMatch(*compileOk("(a)?\\1b"), "b") == "b");
}

TEST_CASE("a repeated group clears its captures each turn") {
    // 22.2.2.5.1 step 4: the second turn must not inherit the first turn's
    // capture, so group 1 is undefined after matching "ab".
    CHECK(capture(*compileOk("(?:(a)|b)*"), "ab", 1) == "<undefined>");
    CHECK(capture(*compileOk("(?:(a)|b)*"), "ba", 1) == "a");
}

TEST_CASE("lookahead is atomic and its captures survive a positive one") {
    CHECK(firstMatch(*compileOk("a(?=b)"), "ab") == "a");
    CHECK(firstMatch(*compileOk("a(?=b)"), "ac") == "<none>");
    CHECK(firstMatch(*compileOk("a(?!b)"), "ac") == "a");
    CHECK(firstMatch(*compileOk("a(?!b)"), "ab") == "<none>");
    CHECK(capture(*compileOk("(?=(\\d+))\\w+"), "abc 123", 1) == "123");
}

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
// answer (chars.h), and this is that sentence as a test: `\w` holds a unit
// exactly when the unit is one of the basic sixty-three or `canonicalize`
// makes it one. Driven over every code unit, because the two agreed for all
// but two of them and it was those two that were wrong.
TEST_CASE("wordRanges is derived from canonicalize and cannot disagree with it") {
    regex::RangeList basic;
    regex::addRange(basic, '0', '9');
    regex::addRange(basic, 'A', 'Z');
    regex::addRange(basic, '_', '_');
    regex::addRange(basic, 'a', 'z');
    regex::normalizeRanges(basic);

    for (bool ignoreCase : {false, true}) {
        const regex::RangeList& words = regex::wordRanges(ignoreCase);
        size_t disagreements = 0;
        for (uint32_t u = 0; u <= regex::kMaxUnit; ++u) {
            const uint16_t unit = static_cast<uint16_t>(u);
            const bool derived = regex::rangesContain(basic, unit) ||
                                 regex::rangesContain(basic, regex::canonicalize(unit, ignoreCase));
            if (regex::rangesContain(words, unit) != derived) ++disagreements;
        }
        CHECK(disagreements == 0);
    }
    // And the derivation comes out empty, which is 22.2.2.7.1 step 3 for an
    // engine with no `u` flag: the two sets are the same sixty-three.
    CHECK(regex::wordRanges(true).size() == regex::wordRanges(false).size());
    CHECK(regex::wordRanges(true).size() == 4);
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
    CHECK(error.find("no Unicode case tables") != std::string::npos);
    // Without `i` the same pattern is ordinary.
    CHECK(compileOk("\\u1E9E") != nullptr);
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

TEST_CASE("a lookbehind matches its Disjunction backward (22.2.2.6)") {
    CHECK(firstMatch(*compileOk("(?<=a)b"), "ab") == "b");
    CHECK(firstMatch(*compileOk("(?<=a)b"), "cb") == "<none>");
    CHECK(firstMatch(*compileOk("(?<!a)b"), "ab") == "<none>");
    CHECK(firstMatch(*compileOk("(?<!a)b"), "cb") == "b");
    // An Alternative's terms run last to first, so the group written FIRST is
    // the one that ends up leftmost — and the one written second is the one
    // that gets to be greedy first.
    auto letters = compileOk("(?<=([ab]+)([bc]+))$");
    CHECK(capture(*letters, "abc", 1) == "a");
    CHECK(capture(*letters, "abc", 2) == "bc");
    auto digits = compileOk("(?<=(\\d+)(\\d+))$");
    CHECK(capture(*digits, "1053", 1) == "1");
    CHECK(capture(*digits, "1053", 2) == "053");
    // A capture closed backward opened at the HIGHER index, so 22.2.2.8 orders
    // the pair rather than assigning it in the order it was written.
    CHECK(capture(*compileOk("(?<=(ab))c"), "abc", 1) == "ab");
    // A backreference backward is compared against the text ENDING at the
    // position. Reversing the terms means `\1` is reached BEFORE the group it
    // names, so it is still an unparticipating group and matches the empty
    // string (22.2.2.9 step 2) — the group then takes the two units behind it.
    CHECK(firstMatch(*compileOk("(?<=(a)\\1)b"), "aab") == "b");
    CHECK(capture(*compileOk("(?<=(ab)\\1)c"), "ababc", 1) == "ab");
    // `\b` and `^` consume nothing whichever direction they ran.
    CHECK(firstMatch(*compileOk("(?<=\\bfoo)bar"), "foobar") == "bar");
    CHECK(firstMatch(*compileOk("(?<=\\bfoo)bar"), "xfoobar") == "<none>");
    CHECK(firstMatch(*compileOk("(?<=^ab)c"), "abc") == "c");
    CHECK(firstMatch(*compileOk("(?<=^b)c"), "abc") == "<none>");
    // A Disjunction is ordered left to right either way: only the terms WITHIN
    // an Alternative reverse.
    CHECK(firstMatch(*compileOk("(?<=ab|b)c"), "abc") == "c");
    CHECK(firstMatch(*compileOk("(?<=cd|b)c"), "abc") == "c");
    // A negative lookbehind discards what its body captured, like a negative
    // lookahead — and a lookbehind nested in a lookahead runs backward from
    // where the outer one reached, after which the outer resumes forward.
    CHECK(capture(*compileOk("(?<!(a))b"), "cb", 1) == "<undefined>");
    CHECK(firstMatch(*compileOk("a(?=b(?<=ab))"), "ab") == "a");
    CHECK(firstMatch(*compileOk("a(?=b(?<=xb))"), "ab") == "<none>");
}

TEST_CASE("a quantified single-unit atom is counted backward too") {
    CHECK(firstMatch(*compileOk("(?<=a{2,3})b"), "aab") == "b");
    CHECK(firstMatch(*compileOk("(?<=a{2,3})b"), "ab") == "<none>");
    CHECK(firstMatch(*compileOk("(?<=a*)b"), "b") == "b");
    // Lazy backward still takes the fewest turns it can.
    CHECK(capture(*compileOk("(?<=(a+?))b"), "aaab", 1) == "a");
    // The same guarantee the forward path carries: 50 000 repetitions is far
    // past the continuation-depth limit, so a backward walk that recursed per
    // repetition would be a named depth error rather than a match. `from` puts
    // the one attempt at the `b`, since the scan itself is not what is tested.
    std::string big(50000, 'a');
    big += "b";
    CHECK(firstMatch(*compileOk("(?<=a*)b"), big, big.size() - 1) == "b");
    CHECK(firstMatch(*compileOk("(?<=[a]{4,})b"), big, big.size() - 1) == "b");
}

TEST_CASE("refused constructs are named, never silently reinterpreted") {
    // A lookbehind is an Assertion, and 22.2.1's Term production has no
    // `Assertion Quantifier` — not even under Annex B, which allows it for a
    // lookahead only.
    CHECK(compileError("(?<=a)*").find("assertion") != std::string::npos);
    CHECK(compileError("(?<=a").find("`)` was expected") != std::string::npos);
    CHECK(compileError("\\p{L}").find("unicode property escapes") != std::string::npos);
    CHECK(compileError("\\q").find("not an escape sequence") != std::string::npos);
    CHECK(compileError("\\1").find("has 0") != std::string::npos);
    CHECK(compileError("a)").find("unmatched") != std::string::npos);
    CHECK(compileError("(a").find("`)` was expected") != std::string::npos);
    CHECK(compileError("[a").find("`]` was expected") != std::string::npos);
    CHECK(compileError("*a").find("nothing to repeat") != std::string::npos);
    CHECK(compileError("^*").find("assertion") != std::string::npos);
    CHECK(compileError("a{3,2}").find("minimum exceeds") != std::string::npos);
    CHECK(compileError("[z-a]").find("start is after its end") != std::string::npos);
    CHECK(compileError("\\k<nope>").find("names no capture group") != std::string::npos);
}

// ---- the `u` flag ----------------------------------------------------------
//
// 22.2.1 takes a +UnicodeMode parameter, and everything below follows from the
// one change it makes: the alphabet is the code POINT rather than the UTF-16
// code unit. The tests are written as pairs wherever the answer differs, so
// each one says what `u` did rather than only that it worked.

TEST_CASE("the code-point seam decodes the same boundaries in both directions") {
    const regex::Units astral = textOf({'a', 0x1F600, 'b'});  // a D83D DE00 b
    // Forward: the pair is one character under `u` and its lead alone without.
    CHECK(regex::codePointAt(astral, 1, true).code == 0x1F600);
    CHECK(regex::codePointAt(astral, 1, true).width == 2);
    CHECK(regex::codePointAt(astral, 1, false).code == 0xD83D);
    CHECK(regex::codePointAt(astral, 1, false).width == 1);
    // Backward: the character ENDING at 3 is the same character, which is what
    // a lookbehind over astral text depends on. Reading `pos - 1` alone would
    // answer with the trailing surrogate and step one unit instead of two.
    CHECK(regex::codePointBefore(astral, 3, true).code == 0x1F600);
    CHECK(regex::codePointBefore(astral, 3, true).width == 2);
    CHECK(regex::codePointBefore(astral, 3, false).code == 0xDE00);
    CHECK(regex::codePointBefore(astral, 3, false).width == 1);
    // The two agree about every boundary: walking forward and walking back
    // over the same string visits the same set of indices.
    CHECK(regex::codePointAt(astral, 0, true).width == 1);
    CHECK(regex::codePointBefore(astral, 1, true).code == 'a');

    // A LONE surrogate is a character in its own right — not U+FFFD, and not
    // half of something. Both directions say so.
    const regex::Units lead = unitsOf({0xD83D, 'x'});
    CHECK(regex::codePointAt(lead, 0, true).code == 0xD83D);
    CHECK(regex::codePointAt(lead, 0, true).width == 1);
    const regex::Units trail = unitsOf({'x', 0xDE00});
    CHECK(regex::codePointBefore(trail, 2, true).code == 0xDE00);
    CHECK(regex::codePointBefore(trail, 2, true).width == 1);
    // A trailing surrogate with no lead before it, at the very start.
    const regex::Units orphan = unitsOf({0xDE00});
    CHECK(regex::codePointBefore(orphan, 1, true).code == 0xDE00);
    CHECK(regex::codePointBefore(orphan, 1, true).width == 1);
}

TEST_CASE("AdvanceStringIndex steps over a character, not a unit (22.2.7.3)") {
    const regex::Units astral = textOf({'a', 0x1F600, 'b'});
    CHECK(regex::advanceStringIndex(astral, 0, true) == 1);
    CHECK(regex::advanceStringIndex(astral, 1, true) == 3);
    CHECK(regex::advanceStringIndex(astral, 1, false) == 2);
    CHECK(regex::advanceStringIndex(astral, 3, true) == 4);
    // Past the end it still moves, so no loop built on it can stall.
    CHECK(regex::advanceStringIndex(astral, 4, true) == 5);
    // A lead surrogate not followed by a trail is one character wide.
    CHECK(regex::advanceStringIndex(unitsOf({0xD83D, 'x'}), 0, true) == 1);
}

TEST_CASE("under `u` an atom consumes one code point") {
    const regex::Units astral = textOf({0x1F600});
    CHECK(extent(*compileOk("^.$", "u"), astral) == "0..2");
    CHECK(extent(*compileOk("^.$"), astral) == "<none>");
    // `\u{...}` is spellable only under `u`, and names the whole character.
    CHECK(extent(*compileOk("\\u{1F600}", "u"), textOf({'a', 0x1F600, 'b'})) == "1..3");
    // 22.2.1 RegExpUnicodeEscapeSequence: a LeadSurrogate escape followed by a
    // TrailSurrogate escape is ONE code point under `u`, so `^...$` holds.
    CHECK(extent(*compileOk("^\\uD83D\\uDE00$", "u"), astral) == "0..2");
    // A class member and a negated class are characters too. `[^x]` under `u`
    // covers the astral plane because the answer is inverted AFTER a membership
    // test the code point simply fails.
    CHECK(extent(*compileOk("^[^x]$", "u"), astral) == "0..2");
    CHECK(extent(*compileOk("^[^x]$"), astral) == "<none>");
    // `\D` is built by COMPLEMENTING, so its ceiling is the mode's: a complement
    // that stopped at 0xFFFF would exclude every astral character from a set
    // whose whole meaning is "not a digit".
    CHECK(extent(*compileOk("^\\D$", "u"), astral) == "0..2");
    CHECK(extent(*compileOk("^\\W$", "u"), astral) == "0..2");
    CHECK(extent(*compileOk("^\\S$", "u"), astral) == "0..2");
    // A range may span above U+FFFF, as one interval and not as surrogate
    // halves — which would also match every other astral character sharing a
    // lead.
    CHECK(matches(*compileOk("[\\u{1F600}-\\u{1F64F}]", "u"), textOf({0x1F607})));
    CHECK_FALSE(matches(*compileOk("[\\u{1F600}-\\u{1F64F}]", "u"), textOf({0x1F650})));
    CHECK_FALSE(matches(*compileOk("[\\u{1F600}-\\u{1F64F}]", "u"), textOf({0x1F5FF})));
    // An unpaired surrogate in the SUBJECT still matches as itself.
    CHECK(extent(*compileOk("^.$", "u"), unitsOf({0xD83D})) == "0..1");
    CHECK(matches(*compileOk("\\uDE00", "u"), unitsOf({'x', 0xDE00})));
    // ...but the trailing half of a real pair is not a position the scan
    // visits, so the same pattern does not find one inside an astral character.
    CHECK_FALSE(matches(*compileOk("\\uDE00", "u"), astral));
    CHECK(matches(*compileOk("\\uDE00"), astral));

    // An astral character written directly in the pattern text is ONE atom
    // under `u`, so a quantifier repeats the whole of it. Without `u` the same
    // text is two atoms and the `+` repeats only the trailing surrogate.
    CHECK(extent(*compileUnitsOk(textOf({0x1F600, '+'}), "u"), textOf({0x1F600, 0x1F600})) ==
          "0..4");
    CHECK(extent(*compileUnitsOk(textOf({0x1F600, '+'}), ""), textOf({0x1F600, 0x1F600})) ==
          "0..2");
}

TEST_CASE("under `u` a lookbehind reads the code point BEFORE the position") {
    const regex::Units astral = textOf({0x1F600, 'x'});
    CHECK(extent(*compileOk("(?<=\\u{1F600})x", "u"), astral) == "2..3");
    // The discriminator: a backward read that took `pos - 1` alone would see
    // the TRAILING surrogate and answer this one true.
    CHECK(extent(*compileOk("(?<=\\uDE00)x", "u"), astral) == "<none>");
    CHECK(extent(*compileOk("(?<=\\uDE00)x"), astral) == "2..3");
    // ...and would leave the position one unit too high for what precedes it,
    // which `^` catches: one character back from `x` is the start of the string
    // under `u` and the middle of a surrogate pair without it.
    CHECK(extent(*compileOk("(?<=^.)x", "u"), astral) == "2..3");
    CHECK(extent(*compileOk("(?<=^.)x"), astral) == "<none>");
    CHECK(extent(*compileOk("(?<=^..)x"), astral) == "2..3");
    // A negative lookbehind follows, since it is the same body.
    CHECK(extent(*compileOk("(?<!\\u{1F600})x", "u"), astral) == "<none>");
    CHECK(extent(*compileOk("(?<!\\uDE00)x", "u"), astral) == "2..3");
}

TEST_CASE("a counted quantifier walks characters, not units, under `u`") {
    // Forward, greedy, giving repetitions BACK: `.*` takes both characters and
    // must hand exactly one back, which is two units and not one.
    const regex::Units two = textOf({0x1F600, 0x1F600});
    {
        regex::MatchResult result;
        std::string error;
        auto pattern = compileOk("(.*)\\u{1F600}", "u");
        REQUIRE(regex::search(*pattern, two, 0, result, error) == regex::ExecStatus::Match);
        CHECK(result.captures[2] == 0);
        CHECK(result.captures[3] == 2);
        CHECK(result.end() == 4);
    }
    // Backward, taking repetitions: three characters precede the `x`, and one
    // of them has to be the leading `\u{1F600}` — so `.{2}` fits and `.{3}`
    // cannot. Counting units instead would make `.{5}` the one that fits.
    const regex::Units three = textOf({0x1F600, 0x1F600, 0x1F600, 'x'});
    CHECK(extent(*compileOk("(?<=\\u{1F600}.{2})x", "u"), three) == "6..7");
    CHECK(extent(*compileOk("(?<=\\u{1F600}.{3})x", "u"), three) == "<none>");
    // The same question without `u`, where the leading character has to be
    // spelled as its two units and the `.` count is a count of units: four of
    // them precede the pair the lookbehind names, not two.
    CHECK(extent(*compileOk("(?<=\\uD83D\\uDE00.{4})x"), three) == "6..7");
    CHECK(extent(*compileOk("(?<=\\uD83D\\uDE00.{5})x"), three) == "<none>");
    // Backward, lazy, taking the fewest it can and still landing on a boundary.
    CHECK(extent(*compileOk("(?<=\\u{1F600}{1,2}?)x", "u"), three) == "6..7");
    CHECK(extent(*compileOk("(?<=^\\u{1F600}{1,2})x", "u"), three) == "<none>");
    CHECK(extent(*compileOk("(?<=^\\u{1F600}{1,3})x", "u"), three) == "6..7");
    // The counted path is still a scan and not a frame per repetition: 20 000
    // astral characters is far past the continuation-depth limit in both
    // directions.
    regex::Units big;
    for (int i = 0; i < 20000; ++i) {
        const regex::Units one = textOf({0x1F600});
        big.append(one);
    }
    big.push_back('b');
    CHECK(extent(*compileOk("^.*b$", "u"), big) == "0..40001");
    CHECK(extent(*compileOk("(?<=.*)b", "u"), big, big.size() - 1) == "40000..40001");
}

TEST_CASE("`u` switches the Annex B leniencies off") {
    // Each of these is a legal pattern WITHOUT `u` — that is what makes them
    // leniencies — and 22.2.1's +UnicodeMode grammar has no production for any
    // of them. Diagnosed by name at the literal, never reinterpreted.
    CHECK(compileOk("a{") != nullptr);
    CHECK(compileError("a{", "u").find("does not begin a quantifier") != std::string::npos);
    CHECK(compileError("a{2", "u").find("does not begin a quantifier") != std::string::npos);
    CHECK(compileOk("]") != nullptr);
    CHECK(compileError("]", "u").find("a lone `]`") != std::string::npos);
    CHECK(compileOk("}") != nullptr);
    CHECK(compileError("}", "u").find("a lone `}`") != std::string::npos);
    CHECK(compileOk("\\-") != nullptr);
    CHECK(compileError("\\-", "u").find("not a valid escape under the `u` flag") !=
          std::string::npos);
    // What +UnicodeMode's IdentityEscape DOES take: a SyntaxCharacter or `/`.
    // And ClassEscape adds `-`, so `[\-]` stays writable.
    CHECK(compileOk("\\$\\{\\}\\]\\/", "u") != nullptr);
    CHECK(compileOk("[\\-]", "u") != nullptr);
    // A valid quantifier is still a quantifier, and a class is still a class.
    CHECK(extent(*compileOk("a{2,3}", "u"), u("aaaa")) == "0..3");
    CHECK(extent(*compileOk("[\\]]", "u"), u("x]y")) == "1..2");
    // `\u{...}` is a code point escape only under `u`. Without it the sequence
    // is Annex B's quantified `\u`, which bronze has never implemented and
    // still refuses by name rather than silently reading one way or the other.
    CHECK(compileError("\\u{2}").find("only under the `u` flag") != std::string::npos);
    CHECK(compileError("\\u{}", "u").find("at least one hexadecimal digit") != std::string::npos);
    CHECK(compileError("\\u{1F600", "u").find("closed by `}`") != std::string::npos);
    CHECK(compileError("\\u{110000}", "u").find("above U+10FFFF") != std::string::npos);
}

TEST_CASE("the refusals `u` does not remove") {
    // `\p{...}` is legal JavaScript under `u` and bronze still refuses it: the
    // honest answer is that it has no UAX #44 table, not that the flag is
    // missing — and reading `\p{L}` as the letter `p` is the silent wrong
    // answer the refusal exists to prevent.
    CHECK(compileError("\\p{L}", "u").find("unicode property escapes") != std::string::npos);
    CHECK(compileError("\\p{L}", "u").find("UAX #44") != std::string::npos);
    CHECK(compileError("\\P{L}", "u").find("unicode property escapes") != std::string::npos);
    CHECK(compileError("\\p{L}").find("unicode property escapes") != std::string::npos);
    // `u` with `i` is refused at the flags, so it never reaches a pattern.
    regex::Flags flags;
    std::string error;
    CHECK_FALSE(regex::parseFlags("ui", flags, error));
    CHECK(error.find("`u` and `i` flags together") != std::string::npos);
}

TEST_CASE("sticky matching only ever tries one index") {
    auto pattern = compileOk("b");
    regex::MatchResult result;
    std::string error;
    const regex::Units input = u("ab");
    CHECK(regex::matchAt(*pattern, input, 0, result, error) == regex::ExecStatus::NoMatch);
    CHECK(regex::matchAt(*pattern, input, 1, result, error) == regex::ExecStatus::Match);
}

TEST_CASE("a quantified single-unit atom does not recurse per input unit") {
    // A naive backtracker needs one stack frame per repetition here; the
    // counted path is what makes this a scan. 50 000 units is far past the
    // continuation-depth limit, so this failing would be a stack overflow.
    std::string big(50000, 'a');
    big += "b";
    CHECK(firstMatch(*compileOk(".*b"), big).size() == big.size());
    CHECK(firstMatch(*compileOk("[a]*b"), big).size() == big.size());
}

TEST_CASE("catastrophic backtracking is a named error, not a hang") {
    regex::MatchResult result;
    std::string error;
    auto pattern = compileOk("(a+)+$");
    const regex::Units input = u("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab");
    const regex::ExecStatus status = regex::search(*pattern, input, 0, result, error);
    CHECK(status == regex::ExecStatus::Error);
    CHECK(error.find("backtracking") != std::string::npos);
}

TEST_CASE("empty alternatives and empty repeats terminate") {
    CHECK(firstMatch(*compileOk("(a|)"), "b") == "");
    CHECK(firstMatch(*compileOk("(a*)*"), "b") == "");
    // The empty-iteration guard (22.2.2.5.1 step 2.a) fails the turn that
    // consumed nothing, which backtracks INTO the other alternative rather
    // than ending the loop — so this consumes the whole input.
    CHECK(firstMatch(*compileOk("(|a)*"), "aa") == "aa");
}
