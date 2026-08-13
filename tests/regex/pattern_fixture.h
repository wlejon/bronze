#pragma once

// The fixtures both regex suites drive the module through: a pattern compiled
// from ASCII or from code units, and the four shapes an answer is read back
// in -- did it match, what did it match, where, and what did a group capture.
//
// Its own header because the suite is split by what it asks ABOUT, not by what
// it needs to ask with: `regex_test.cpp` is the grammar and the matcher, and
// `chars_test.cpp` is the character data underneath them. Both need to compile
// a pattern and run it, and a second copy of that would be a second set of
// conventions for reading a failure.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "regex/chars.h"
#include "regex/regex.h"

namespace bronze::regex_fixture {

using namespace bronze;

inline regex::Units u(std::string_view ascii) {
    regex::Units out;
    for (char c : ascii) out.push_back(static_cast<uint16_t>(static_cast<unsigned char>(c)));
    return out;
}

inline regex::Flags flagsOf(std::string_view text) {
    regex::Flags flags;
    std::string error;
    REQUIRE(regex::parseFlags(text, flags, error));
    return flags;
}

inline regex::PatternPtr compileOk(std::string_view source, std::string_view flagText = "") {
    std::string error;
    auto pattern = regex::compile(u(source), flagsOf(flagText), error);
    REQUIRE_MESSAGE(pattern != nullptr, error);
    return pattern;
}

inline std::string compileError(std::string_view source, std::string_view flagText = "") {
    std::string error;
    auto pattern = regex::compile(u(source), flagsOf(flagText), error);
    CHECK(pattern == nullptr);
    return error;
}

// The whole match's text, or "<none>". Written as a string so a failing case
// reads as the thing the pattern was supposed to find.
inline std::string firstMatch(const regex::Pattern& pattern, std::string_view input, size_t from = 0) {
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
inline regex::Units unitsOf(std::initializer_list<uint32_t> codes) {
    regex::Units out;
    for (uint32_t c : codes) out.push_back(static_cast<char16_t>(c));
    return out;
}

// A string spelled by CODE POINT, with anything above 0xFFFF encoded as the
// surrogate pair a JavaScript string holds for it. Used for both subjects and
// pattern text, since under `u` a pattern may spell an astral character
// directly.
inline regex::Units textOf(std::initializer_list<uint32_t> codes) {
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

inline regex::PatternPtr compileUnitsOk(const regex::Units& source, std::string_view flagText) {
    std::string error;
    auto pattern = regex::compile(source, flagsOf(flagText), error);
    REQUIRE_MESSAGE(pattern != nullptr, error);
    return pattern;
}

// The half-open extent of the first match, as "start..end". A `u` question is
// almost always about how far the index moved, which the matched TEXT cannot
// show and this can.
inline std::string extent(const regex::Pattern& pattern, const regex::Units& input, size_t from = 0) {
    regex::MatchResult result;
    std::string error;
    if (regex::search(pattern, input, from, result, error) != regex::ExecStatus::Match) {
        return error.empty() ? "<none>" : "<error: " + error + ">";
    }
    return std::to_string(result.start()) + ".." + std::to_string(result.end());
}

inline bool matches(const regex::Pattern& pattern, const regex::Units& input) {
    regex::MatchResult result;
    std::string error;
    return regex::search(pattern, input, 0, result, error) == regex::ExecStatus::Match;
}

inline std::string capture(const regex::Pattern& pattern, std::string_view input, uint32_t group) {
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

}  // namespace bronze::regex_fixture
