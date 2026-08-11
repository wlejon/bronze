#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Regular expressions: the pattern grammar of ECMA-262 22.2.1 and the
// backtracking matcher of 22.2.2, and nothing else.
//
// Its own module for the reason `src/json` is one: this is a SECOND grammar,
// with its own escapes, its own precedence and — decisively — its own
// rejections. `\d` is a digit class here and the character `d` in a string
// literal; `{2,3}` is a quantifier here and three tokens in JavaScript. A
// parser shared between two grammars that differ in what they reject is how
// the rejections get lost.
//
// It knows nothing about bronze's value model — no Value, no heap, no GC — so
// `tests/regex` can drive it directly and the matcher's decisions are provable
// without a runtime. `src/runtime/builtin_regexp.cpp` is what turns a match
// into JavaScript values.

namespace bronze::regex {

// UTF-16 CODE UNITS, because that is what a JavaScript string is (docs/0004)
// and because every position a match reports is a code unit index.
using Units = std::u16string;
using UnitsView = std::u16string_view;

// `g i m s y`, and nothing else — `u`, `v` and `d` are refused by name at
// compile time (docs/0024).
struct Flags {
    bool global = false;
    bool ignoreCase = false;
    bool multiline = false;
    bool dotAll = false;
    bool sticky = false;

    // The flags in the order 22.2.6.5 pins for `RegExp.prototype.flags`,
    // which is the order `source`/`flags` and `toString` all report.
    std::string text() const;
};

// Parses a flags string. Returns false and fills `error` for an unknown
// letter, a repeated one, or one bronze does not implement.
bool parseFlags(std::string_view text, Flags& out, std::string& error);

// A compiled pattern, opaque here. The tree it holds is 22.2.1's and belongs
// to the matcher; a consumer that could walk it would freeze the shape of
// every node. That is also why the deleter is out of line — owning a
// `unique_ptr` to it must not require seeing the tree.
class Pattern;
struct PatternDeleter {
    void operator()(Pattern* pattern) const noexcept;
};
using PatternPtr = std::unique_ptr<Pattern, PatternDeleter>;

// Compiles one complete pattern. Returns null and fills `error` on a syntax
// error or on a construct bronze refuses; the message names what it is, in the
// house style. Every input unit is consumed or the compile fails — a pattern
// whose tail was silently ignored would match things it does not describe.
PatternPtr compile(UnitsView source, const Flags& flags, std::string& error);

// The flags a pattern was compiled with, and its group bookkeeping.
const Flags& patternFlags(const Pattern& pattern);
uint32_t captureCount(const Pattern& pattern);   // excludes capture 0
bool hasNamedGroups(const Pattern& pattern);
// The name of capture `index` (1-based), or the empty string.
const std::string& groupName(const Pattern& pattern, uint32_t index);

// A match: two entries per capture, `[2i]` and `[2i + 1]`, with capture 0 the
// whole match. `kUnset` for a group that did not participate — which is not the
// same as one that matched empty, and is the difference between `undefined` and
// `""` in the array `exec` builds.
struct MatchResult {
    static constexpr int64_t kUnset = -1;
    std::vector<int64_t> captures;

    int64_t start() const { return captures.empty() ? kUnset : captures[0]; }
    int64_t end() const { return captures.size() < 2 ? kUnset : captures[1]; }
};

enum class ExecStatus {
    NoMatch,
    Match,
    // The matcher gave up: a construct whose correct answer needs Unicode data
    // bronze does not carry, or a backtracking budget exhausted. `error` names
    // which. Never a wrong answer reported as a match or a miss.
    Error,
};

// One attempt at exactly `at`, with no scan. This is `y`'s whole semantics and
// the primitive the scanning form is built from.
ExecStatus matchAt(const Pattern& pattern, UnitsView input, size_t at, MatchResult& out,
                   std::string& error);

// The first match at or after `from`, scanning forward. `sticky` is the
// caller's to honour: `exec` on a sticky pattern uses `matchAt`.
ExecStatus search(const Pattern& pattern, UnitsView input, size_t from, MatchResult& out,
                  std::string& error);

}  // namespace bronze::regex
