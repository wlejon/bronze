// The pattern grammar and the matcher, driven directly. Nothing here needs a
// heap, a Value or a compiled program, which is the point of the module being
// its own static library: every question below is decided by ECMA-262 22.2
// alone.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

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

TEST_CASE("a case fold bronze has no table for is a named error, never a guess") {
    const std::string error = compileError("\\u03A9", "i");
    CHECK(error.find("U+03A9") != std::string::npos);
    CHECK(error.find("no Unicode case tables") != std::string::npos);
    // Without `i` the same pattern is ordinary.
    CHECK(compileOk("\\u03A9") != nullptr);
}

TEST_CASE("refused constructs are named, never silently reinterpreted") {
    CHECK(compileError("(?<=a)b").find("lookbehind") != std::string::npos);
    CHECK(compileError("(?<!a)b").find("lookbehind") != std::string::npos);
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
