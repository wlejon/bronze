// The `v` flag's character class: 22.2.1's ClassSetExpression, and what it
// reserves.
//
// Two things are pinned here and they pull in opposite directions. The first is
// that the grammar computes — a class is a set EXPRESSION, so `[[a-z]--[aeiou]]`
// is a difference and `[[0-9a-f]&&[a-z]]` an intersection, and both compose with
// nesting and with a complement. The second is that it REFUSES: every character
// that could begin a future operator is a syntax error written bare, which is
// the only way a pattern spelled for a later edition cannot quietly match under
// this one. A test suite that only checked the first half would let the second
// rot away one lenient reading at a time.
//
// Case folding under `v` with `i` has its own section at the bottom, because it
// is not the same operation `u` performs and the difference is observable.

#include <doctest/doctest.h>

#include "pattern_fixture.h"
#include "regex/chars.h"

using namespace bronze::regex_fixture;
namespace regex = bronze::regex;

namespace {

// Which members of a small alphabet a pattern accepts, as a string, so a
// failing set operation reads as the set it produced rather than as a boolean.
std::string accepted(const regex::Pattern& pattern, std::string_view alphabet) {
    std::string out;
    for (char c : alphabet) {
        const std::string one(1, c);
        if (matches(pattern, u(one))) out.push_back(c);
    }
    return out;
}

}  // namespace

TEST_CASE("a `v` class is a set expression: union, difference, intersection") {
    // Union is still juxtaposition, and a nested class is one member of it.
    CHECK(accepted(*compileOk("^[[a-c][x-z]]$", "v"), "abcwxyz") == "abcxyz");

    // ClassSubtraction. The left operand is a nested class here and a bare
    // range would not parse: `--` takes ClassSetOperands, and a range is not
    // one.
    CHECK(accepted(*compileOk("^[[a-j]--[aeiou]]$", "v"), "abcdefghij") == "bcdfghj");

    // ClassIntersection.
    CHECK(accepted(*compileOk("^[[0-9a-f]&&[a-z]]$", "v"), "0159abcfgz") == "abcf");

    // Both operators are left-associative chains, not two-operand productions.
    CHECK(accepted(*compileOk("^[[a-f]--[a]--[b]]$", "v"), "abcdef") == "cdef");
    CHECK(accepted(*compileOk("^[[a-f]&&[b-f]&&[c-f]]$", "v"), "abcdef") == "cdef");

    // A complement composes with an intersection, which is the form the
    // proposal's own examples are written in.
    CHECK(accepted(*compileOk("^[[a-f]&&[^aeiou]]$", "v"), "abcdef") == "bcdf");

    // And a nested class may itself be an operation.
    CHECK(accepted(*compileOk("^[[[a-f]--[b]]&&[a-c]]$", "v"), "abcdef") == "ac");
}

TEST_CASE("`--` and `&&` do not mix without a nesting to say which came first") {
    // 22.2.1 gives ClassSubtraction and ClassIntersection separate productions
    // and no precedence between them, so this has no parse at all. bronze says
    // so, and says what to write instead, rather than picking one.
    const std::string mixed = compileError("[[a]--[b]&&[c]]", "v");
    CHECK(mixed.find("cannot be mixed") != std::string::npos);
    CHECK(compileError("[[a]&&[b]--[c]]", "v").find("cannot be mixed") != std::string::npos);

    // The nesting that resolves it parses.
    CHECK(accepted(*compileOk("^[[[a-f]--[b]]&&[b-d]]$", "v"), "abcdef") == "cd");
}

TEST_CASE("the `v` class reserves the characters a later operator could use") {
    // ClassSetSyntaxCharacter: `( ) [ ] { } / - \ |`. Each is an ordinary
    // member of a class in every other mode, and each is a syntax error here.
    for (const char* bare : {"[(]", "[)]", "[{]", "[}]", "[/]", "[|]"}) {
        const std::string message = compileError(bare, "v");
        CHECK(message.find("must be escaped inside a `v`-mode character class") !=
              std::string::npos);
    }
    // The same text without the flag is the character, which is what makes the
    // refusal a real change and not a tightening of something already broken.
    CHECK(matches(*compileOk("^[(]$", ""), u("(")));
    CHECK(matches(*compileOk("^[(]$", "u"), u("(")));

    // Escaped, they are the characters again — ClassSetCharacter's
    // `\ CharacterEscape` and `\ ClassSetReservedPunctuator` alternatives.
    CHECK(matches(*compileOk("^[\\(]$", "v"), u("(")));
    CHECK(matches(*compileOk("^[\\|]$", "v"), u("|")));
    CHECK(matches(*compileOk("^[\\-]$", "v"), u("-")));

    // ClassSetReservedPunctuator alone is the character; DOUBLED it is
    // reserved. `&&` is the operator that exists today and the rest are held
    // for the ones that do not.
    CHECK(matches(*compileOk("^[&]$", "v"), u("&")));
    CHECK(matches(*compileOk("^[!]$", "v"), u("!")));
    CHECK(matches(*compileOk("^[~]$", "v"), u("~")));
    for (const char* doubled : {"[!!]", "[##]", "[$$]", "[**]", "[..]", "[::]", "[~~]"}) {
        const std::string message = compileError(doubled, "v");
        CHECK(message.find("reserved double punctuator") != std::string::npos);
    }
    // Escaping either half writes the two characters.
    CHECK(matches(*compileOk("^[\\!\\!]+$", "v"), u("!!")));

    // A trailing `-` is a member in ClassRanges and a reserved character here.
    CHECK(matches(*compileOk("^[a-]+$", ""), u("a-")));
    CHECK(compileError("[a-]", "v").find("must be escaped") != std::string::npos);
}

TEST_CASE("`\\q{...}` and the properties of strings are refused together, by name") {
    // They are one feature — a CharSet whose members are not single characters
    // — and bronze implements neither. The refusal names both halves so that a
    // reader who arrived from either one finds the other.
    const std::string q = compileError("[\\q{ab}]", "v");
    CHECK(q.find("ClassStringDisjunction") != std::string::npos);
    CHECK(q.find("properties of strings") != std::string::npos);
    // Even the empty disjunction, which is a set containing the empty string.
    CHECK(compileError("[\\q{}]", "v").find("ClassStringDisjunction") != std::string::npos);

    // The other half, and the reason it can be named exactly where an unknown
    // binary property cannot: 22.2.1's Table 67 is a closed list of seven, so a
    // reader who spelled `RGI_Emoji` correctly is told the feature is missing
    // rather than told to check their spelling.
    const std::string strings = compileError("\\p{RGI_Emoji}", "v");
    CHECK(strings.find("property of STRINGS") != std::string::npos);
    CHECK(strings.find("\\q{...}") != std::string::npos);
    CHECK(compileError("[\\p{Basic_Emoji}]", "v").find("property of STRINGS") !=
          std::string::npos);
    // A binary property of CHARACTERS keeps the message it had, because bronze
    // has no list of those and cannot tell a missing one from a typo.
    CHECK(compileError("\\p{Emoji}", "v").find("misspelling") != std::string::npos);

    // Outside a `v` class the same two characters are the old refusal about an
    // identity escape, unchanged: `/\q/` has never been legal, for its own
    // reason, and this must not answer that question.
    CHECK(compileError("\\q").find("is not an escape sequence") != std::string::npos);
    CHECK(compileError("[\\q]").find("is not an escape sequence") != std::string::npos);
    CHECK(compileError("\\q{ab}", "v").find("is not an escape sequence") != std::string::npos);
}

TEST_CASE("`v` implies the code point alphabet, and excludes `u`") {
    regex::Flags flags;
    std::string error;
    REQUIRE(regex::parseFlags("v", flags, error));
    CHECK(flags.unicodeSets);
    CHECK_FALSE(flags.unicode);
    CHECK(flags.unicodeMode());

    // Everything +UnicodeMode gives, without the letter `u`: astral ranges as
    // one interval, `\u{...}`, and the property escapes.
    CHECK(matches(*compileUnitsOk(textOf({'^', '[', 0x1F600, '-', 0x1F64F, ']', '$'}), "v"),
                  textOf({0x1F60A})));
    CHECK(matches(*compileOk("^[\\u{1F600}-\\u{1F64F}]$", "v"), textOf({0x1F600})));
    CHECK(matches(*compileOk("^\\p{Script=Greek}$", "v"), unitsOf({0x03B1})));
    // And the strictness: an Annex B identity escape is gone here too.
    CHECK(compileError("\\-", "v").find("not a valid escape under the `u` flag") !=
          std::string::npos);

    // 22.2.3.4 ParsePattern step 1. Both letters are legal on their own, so
    // this is the only place the pair can be diagnosed.
    CHECK_FALSE(regex::parseFlags("uv", flags, error));
    CHECK(error.find("`u` and `v` cannot both be set") != std::string::npos);
}

TEST_CASE("`[]` and `[^]` are the empty set and its complement under `v`") {
    // ClassContents may be empty in both grammars, and `[]` is the one class
    // with no members at all — it matches nothing, and its complement matches
    // everything.
    CHECK_FALSE(matches(*compileOk("[]", "v"), u("a")));
    CHECK(matches(*compileOk("^[^]$", "v"), u("a")));
    CHECK(matches(*compileUnitsOk(textOf({'^', '[', '^', ']', '$'}), "v"), textOf({0x1F600})));
}

TEST_CASE("a `v` class range is still a range, and still checked end to end") {
    CHECK(accepted(*compileOk("^[a-e]$", "v"), "abcdefg") == "abcde");
    // ClassSetRange takes two ClassSetCharacters, so a class escape on either
    // end is not a parse.
    CHECK(compileError("[a-\\d]", "v").find("cannot be the end of a range") != std::string::npos);
    // 22.2.1's early error on ClassSetRange.
    CHECK(compileError("[z-a]", "v").find("start is after its end") != std::string::npos);
    // `\b` inside a class is the backspace here as everywhere else.
    CHECK(matches(*compileOk("^[\\b]$", "v"), unitsOf({0x0008})));
}

// ---- case folding, which `v` does differently ------------------------------

TEST_CASE("`v` with `i` folds the operands of a set operation") {
    // Without the fold an intersection of two spellings of one letter would be
    // empty. MaybeSimpleCaseFolding is what makes them meet.
    CHECK(matches(*compileOk("^[[a]&&[A]]$", "vi"), u("a")));
    CHECK(matches(*compileOk("^[[a]&&[A]]$", "vi"), u("A")));
    // And a difference removes the letter by either spelling.
    CHECK(accepted(*compileOk("^[[a-e]--[C]]$", "vi"), "abcde") == "abde");
    CHECK_FALSE(matches(*compileOk("^[[a-e]--[C]]$", "vi"), u("C")));

    // U+212A KELVIN SIGN folds to `k`, so a class that removes one removes the
    // other: 22.2.2.9 maps the operand through scf before the subtraction.
    CHECK_FALSE(matches(*compileOk("[^\\u212A]", "vi"), u("k")));
    CHECK(matches(*compileOk("[\\u212A]", "vi"), u("k")));
    // Without `i` they are two different characters again.
    CHECK(matches(*compileOk("[^\\u212A]", "v"), u("k")));
}

TEST_CASE("under `v` with `i` a complement is taken over the fold's fixed points") {
    // 22.2.2.9's AllCharacters: with BOTH flags the alphabet a complement runs
    // over is the code points that are their own simple case folding, not the
    // whole code space. That single choice is what makes `[^\P{X}]` come back
    // to `\p{X}` — the identity the `v` flag exists to restore.
    CHECK(matches(*compileOk("[^\\P{Lowercase_Letter}]", "vi"), u("A")));
    CHECK(matches(*compileOk("[^\\P{Lowercase_Letter}]", "vi"), u("a")));
    CHECK_FALSE(matches(*compileOk("[^\\P{Lowercase_Letter}]", "vi"), u("4")));
    // The same pattern under `ui`, where the complement runs over every code
    // point and the inversion happens after canonicalization: it matches
    // nothing at all. This is the bug, pinned as the thing that must not
    // change under the OTHER flag.
    CHECK_FALSE(matches(*compileOk("[^\\P{Lowercase_Letter}]", "ui"), u("A")));
    CHECK_FALSE(matches(*compileOk("[^\\P{Lowercase_Letter}]", "ui"), u("a")));

    // The single escape has the same asymmetry, and for the same reason.
    CHECK_FALSE(matches(*compileOk("\\P{Lowercase_Letter}", "vi"), u("A")));
    CHECK(matches(*compileOk("\\P{Lowercase_Letter}", "ui"), u("A")));

    // `[^k]` is NOT one of the pairs that differ, and it is pinned so that a
    // reader looking for the difference does not conclude it is everywhere:
    // under both flags U+212A canonicalizes onto the `k` that was removed.
    CHECK_FALSE(matches(*compileOk("[^k]", "vi"), unitsOf({0x212A})));
    CHECK_FALSE(matches(*compileOk("[^k]", "ui"), unitsOf({0x212A})));
    // And without `i` the complement is over the whole code space again, so
    // the Kelvin sign is simply another character.
    CHECK(matches(*compileOk("[^k]", "v"), unitsOf({0x212A})));
}

TEST_CASE("the range set operations are set operations") {
    // intersectRanges and subtractRanges exist for `&&` and `--`, and both
    // normalize their inputs, because a class built member by member arrives
    // as an unsorted list with overlaps in it.
    regex::RangeList a;
    regex::addRange(a, 'm', 'p');
    regex::addRange(a, 'a', 'e');
    regex::addRange(a, 'c', 'g');  // overlaps the previous one, out of order

    regex::RangeList b;
    regex::addRange(b, 'd', 'n');

    const regex::RangeList meet = regex::intersectRanges(a, b);
    for (char c : std::string("abcdefghijklmnopqrs")) {
        const bool expected = (c >= 'd' && c <= 'g') || (c >= 'm' && c <= 'n');
        CHECK(regex::rangesContain(meet, static_cast<uint32_t>(c)) == expected);
    }

    const regex::RangeList left = regex::subtractRanges(a, b);
    for (char c : std::string("abcdefghijklmnopqrs")) {
        const bool expected = (c >= 'a' && c <= 'c') || (c >= 'o' && c <= 'p');
        CHECK(regex::rangesContain(left, static_cast<uint32_t>(c)) == expected);
    }

    // The empty cases, which the operators reach whenever a class subtracts
    // itself.
    CHECK(regex::subtractRanges(a, a).empty());
    CHECK(regex::intersectRanges(a, regex::RangeList{}).empty());
    CHECK(regex::subtractRanges(regex::RangeList{}, a).empty());
}
