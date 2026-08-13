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

// A string spelled by code point, for the case table: `u()` above is ASCII
// only, and a fold question is never about a character that fits in it.
regex::Units unitsOf(std::initializer_list<uint32_t> codes) {
    regex::Units out;
    for (uint32_t c : codes) out.push_back(static_cast<char16_t>(c));
    return out;
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
    CHECK_FALSE(regex::parseFlags("u", flags, error));
    CHECK(error.find("`u` flag") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("d", flags, error));
    CHECK(error.find("match indices") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("gg", flags, error));
    CHECK(error.find("more than once") != std::string::npos);
    CHECK_FALSE(regex::parseFlags("q", flags, error));
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
