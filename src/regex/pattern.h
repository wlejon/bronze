#pragma once

#include <memory>
#include <string>
#include <vector>

#include "regex/chars.h"
#include "regex/regex.h"

// The pattern tree ECMA-262 22.2.1 spells out, one node kind per production
// that the matcher has to treat differently. It is the module's internal
// currency: `regex.h` hands out a `Pattern` and never the nodes, because the
// tree is a matcher detail and a consumer that walked it would freeze it.

namespace bronze::regex {

enum class NodeKind : uint8_t {
    // A Disjunction: the alternatives, in source order, tried left to right.
    Alternation,
    // An Alternative: the terms, in source order. An empty one matches the
    // empty string, which is what makes `(a|)` legal.
    Sequence,
    // One code unit, already canonicalized when `i` is set.
    Char,
    // A CharacterClass, or a class escape used on its own. `ranges` is
    // normalized; `negated` is `[^...]`, which is NOT the same as complementing
    // the ranges once `i` is in play (22.2.2.7.1 inverts the ANSWER, after the
    // canonicalizing membership test).
    Class,
    // `.` — every unit but the four line terminators, unless `s` is set.
    Dot,
    // `^`, `$`, `\b`, `\B`.
    Assertion,
    // `(?=...)` / `(?!...)`. Atomic: the spec's continuation for a lookahead
    // accepts immediately, so the inner match is never retried to satisfy what
    // follows it.
    Lookahead,
    // `(?<=...)` / `(?<!...)`. The same node as a lookahead in every respect
    // but one: 22.2.2.6 matches its Disjunction with `direction` = backward, so
    // its terms run right to left from the assertion's position and every
    // position inside it DECREASES. Like a lookahead it is atomic and consumes
    // nothing.
    Lookbehind,
    // `(...)` / `(?:...)` / `(?<name>...)`. `captureIndex` is 0 for a
    // non-capturing group, since capture 0 is the whole match and can never be
    // a group's own.
    Group,
    // `\1` / `\k<name>`, resolved to a capture index at parse time.
    Backreference,
    // An atom with a quantifier. `children[0]` is the atom.
    Repeat,
};

enum class AssertionKind : uint8_t { Start, End, WordBoundary, NotWordBoundary };

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
    NodeKind kind = NodeKind::Sequence;

    uint16_t ch = 0;                  // Char
    RangeList ranges;                 // Class
    bool negated = false;             // Class
    AssertionKind assertion = AssertionKind::Start;
    bool lookaroundNegative = false;  // Lookahead, Lookbehind
    uint32_t captureIndex = 0;        // Group
    uint32_t backreference = 0;       // Backreference

    // Repeat. `max` is kUnbounded for `*`, `+` and `{n,}`.
    uint32_t min = 0;
    uint32_t max = 0;
    bool greedy = true;
    // The capture indices the quantified atom contains, as the half-open range
    // (firstCapture, firstCapture + captureCount]. RepeatMatcher clears them
    // before every iteration (22.2.2.5.1 step 4), which is what makes
    // `/(?:(a)|b)*/.exec("ab")[1]` undefined rather than "a".
    uint32_t firstCapture = 0;
    uint32_t captureCount = 0;
    // Whether the atom consumes exactly one unit and captures nothing, so the
    // repetition can be counted and backtracked over ITERATIVELY. Without this
    // every `.*` would recurse once per input unit and blow the stack on a
    // string of any size.
    bool simpleAtom = false;

    std::vector<NodePtr> children;
};

// A compiled pattern: the tree, the flags it was compiled with, and the group
// bookkeeping `exec` needs to build a match array.
class Pattern {
public:
    NodePtr root;
    Flags flags;
    // Capture groups, excluding capture 0. `groupNames[i]` is the name of
    // capture i + 1, or empty when that group has none.
    uint32_t groupCount = 0;
    std::vector<std::string> groupNames;
    // True when any group has a name, which is what decides whether a match
    // array's `groups` is an object or `undefined` (22.2.7.2 step 8).
    bool hasNamedGroups = false;
};

}  // namespace bronze::regex
