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

// UTF-16 CODE UNITS, because that is what a JavaScript string is and because
// every position a match reports is a code unit index — under `u` as well,
// where the ALPHABET is the code point but the index never is.
using Units = std::u16string;
using UnitsView = std::u16string_view;

// 22.2.7.3 AdvanceStringIndex: the index after `index`, which is two units and
// not one when `unicode` holds and a surrogate PAIR begins there. Every place a
// cursor steps over a character it did not match — an empty match's `lastIndex`,
// `split`'s walk past a non-separator, the scan `search` runs — is this.
size_t advanceStringIndex(UnitsView input, size_t index, bool unicode);

// `d g i m s u v y`, and nothing else. Every combination compiles but one:
// 22.2.3.1 makes `u` with `v` a SyntaxError, because they are two readings of
// the same mode rather than two bits. `u` with `i` is the combination that
// changes what Canonicalize means (22.2.2.9 step 1 folds instead of
// uppercasing) rather than what is legal.
struct Flags {
    // 22.2.6.6 hasIndices. It changes nothing about MATCHING — the capture
    // extents the matcher records are the same either way — and only tells
    // 22.2.7.2 to hand them on as an `indices` array. It is a flag rather than
    // an argument to `exec` because a program reads it back off the RegExp.
    bool hasIndices = false;
    bool global = false;
    bool ignoreCase = false;
    bool multiline = false;
    bool dotAll = false;
    bool unicode = false;
    // 22.2.6.19 unicodeSets. It is `unicode` plus a second grammar for what a
    // character class may say, so it is a separate BIT — a program reads the
    // two accessors apart — and never a separate alphabet.
    bool unicodeSets = false;
    bool sticky = false;

    // Which grammar and which alphabet, which `u` and `v` answer identically:
    // one code POINT per character, `\u{...}`, property escapes, and Annex B's
    // leniencies off. Everything in the module that asks "is this +UnicodeMode?"
    // asks this and not the `u` bit, so a `v` pattern cannot fall back to the
    // code-unit reading in one place while using code points in another.
    bool unicodeMode() const { return unicode || unicodeSets; }

    // The flags in the order 22.2.6.5 pins for `RegExp.prototype.flags`,
    // which is the order `source`/`flags` and `toString` all report.
    std::string text() const;
};

// Parses a flags string. Returns false and fills `error` for an unknown
// letter, a repeated one, or one bronze does not implement. No combination is
// refused: the tables both readings of Canonicalize need are carried.
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
