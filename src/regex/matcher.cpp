// The matcher of ECMA-262 22.2.2: backtracking, with the specification's own
// continuation structure made explicit.
//
// The spec writes a matcher as a function that takes a State and a
// continuation and returns a State or FAILURE, and it means it — the
// continuation is what makes `/a*a/` find the second `a` by giving back one of
// the first ones, and it is what makes a capture inside a repeated group
// visible to the term after the group. Here a continuation is a `Cont`: a node
// on the C++ stack naming what is left to do, chained through `next`. Four
// kinds cover the whole grammar (finish, resume a sequence, take another turn
// round a quantifier, close a capture), and every one of them undoes what it
// wrote when the rest of the match fails, which is what a functional State
// gets for free and a mutable capture vector has to be careful about.
//
// No JIT and no automaton: 22.2.2 is defined by backtracking, and the
// constructs that make an automaton hard (backreferences, lookahead) are
// exactly the ones bronze implements. What IS optimized is the one case where
// naive backtracking is not merely slow but unsound in practice — a quantified
// single-unit atom, which would otherwise recurse once per input unit and
// exhaust the stack on an ordinary `.*` over an ordinary string.
//
// Everything here carries a DIRECTION, which is 22.2.2.6's `direction`
// parameter and exists for exactly one production: a lookbehind matches its
// Disjunction backward. Backward means three things and only three — an
// Alternative's terms are taken last to first, an atom that consumes a
// character reads the one BEFORE the current position and continues there, and
// a group's capture range is ordered rather than assumed. Nothing else changes:
// a quantifier is still greedy, an assertion still consumes nothing, and the
// continuation chain still names what is left to do.
//
// What a character IS comes from the `u` flag, and it enters here at exactly
// two points: `characterAt` / `characterBefore`, which decode one code point in
// each direction, and the quantifier scan, which walks the count back one
// character at a time instead of subtracting it from the index. The direction
// and the alphabet meet in the backward decode — a lookbehind over astral text
// has to see a trailing surrogate at `pos - 1` and step over its LEAD — and
// that meeting is why `codePointBefore` exists rather than a reversed loop.

#include <cstdint>
#include <string>
#include <vector>

#include "regex/pattern.h"

namespace bronze::regex {

namespace {

constexpr uint32_t kUnbounded = 0xFFFFFFFFu;

// How much backtracking a single attempt may do before the matcher gives up
// and says so. A pattern like `/(a+)+b/` against a run of `a`s is exponential
// by construction, and the alternative to a budget is a compiled program that
// appears to hang. Deterministic, so a case that trips it trips it everywhere.
constexpr uint64_t kStepBudget = 20000000;

// How deep the continuation chain may go. Each level is a C++ stack frame, so
// this is a stack-overflow guard with a message rather than a crash. The
// single-unit quantifier path below is what keeps ordinary patterns far from
// it: without that path, `/.*/` over a 10 kB string would need 10 000 levels.
constexpr uint32_t kMaxDepth = 2000;

bool isLineTerminator(uint32_t u) {
    return u == 0x000A || u == 0x000D || u == 0x2028 || u == 0x2029;
}

struct Cont {
    enum class Kind : uint8_t { Done, Sequence, Repeat, Capture };

    Kind kind = Kind::Done;
    const std::vector<NodePtr>* terms = nullptr;  // Sequence
    size_t index = 0;                             // Sequence
    const Node* repeat = nullptr;                 // Repeat
    uint32_t min = 0;                             // Repeat
    uint32_t max = 0;                             // Repeat
    // Repeat: where this turn round the loop began, which is what the
    // empty-iteration guard compares against. Capture: where the group opened.
    size_t mark = 0;
    uint32_t captureIndex = 0;  // Capture
    // Sequence, Repeat: the direction the node that parked this continuation
    // was running in. It rides on the Cont rather than being threaded through
    // `runCont` because a continuation is resumed from wherever the match got
    // to — including from inside a lookbehind whose direction is not the
    // enclosing one.
    bool backward = false;
    const Cont* next = nullptr;
};

class Matcher {
public:
    Matcher(const Pattern& pattern, UnitsView input)
        : pattern_(pattern),
          input_(input),
          ignoreCase_(pattern.flags.ignoreCase),
          unicode_(pattern.flags.unicodeMode()) {
        captures_.assign(static_cast<size_t>(pattern.groupCount + 1) * 2, MatchResult::kUnset);
    }

    ExecStatus run(size_t at, MatchResult& out, std::string& error) {
        captures_.assign(captures_.size(), MatchResult::kUnset);
        steps_ = 0;
        depth_ = 0;
        failed_ = false;
        error_.clear();

        Cont done;
        done.kind = Cont::Kind::Done;
        const bool ok = matchNode(*pattern_.root, at, &done, /*backward=*/false);
        if (failed_) {
            error = error_;
            return ExecStatus::Error;
        }
        if (!ok) return ExecStatus::NoMatch;
        captures_[0] = static_cast<int64_t>(at);
        captures_[1] = static_cast<int64_t>(endPos_);
        out.captures = captures_;
        return ExecStatus::Match;
    }

private:
    const Pattern& pattern_;
    UnitsView input_;
    bool ignoreCase_;
    bool unicode_;
    std::vector<int64_t> captures_;
    size_t endPos_ = 0;
    uint64_t steps_ = 0;
    uint32_t depth_ = 0;
    bool failed_ = false;
    std::string error_;

    bool giveUp(std::string message) {
        if (!failed_) {
            failed_ = true;
            error_ = std::move(message);
        }
        return false;
    }

    // ---- single-character tests ---------------------------------------------

    // Canonicalize over the alphabet, which is the flags' business and not
    // this file's: `chars.cpp` holds both tables and picks between them, so
    // the matcher never learns that there are two.
    uint32_t canonical(uint32_t code) const {
        return canonicalize(code, ignoreCase_, unicode_);
    }

    bool charMatches(uint32_t patternCode, uint32_t inputCode) const {
        // The pattern's character was canonicalized when it was parsed, and one
        // the parser refused to canonicalize can never appear there — so one
        // canonicalization here is the whole comparison.
        return canonical(inputCode) == patternCode;
    }

    // 22.2.2.7.1: found is "the set holds SOME member whose canonicalization is
    // the input's", which is not "the input's canonicalization is in the set".
    // `/[µ]/i` matches U+039C because Canonicalize(U+00B5) is U+039C, and no
    // amount of canonicalizing the input alone finds that.
    bool classMatches(const Node& node, uint32_t inputCode) const {
        bool found = rangesContain(node.ranges, inputCode);
        if (!found && ignoreCase_) {
            const uint32_t cc = canonical(inputCode);
            found = rangesContain(node.ranges, cc);
            if (!found) {
                for (uint32_t candidate : caseCandidates(cc, unicode_)) {
                    if (rangesContain(node.ranges, candidate)) {
                        found = true;
                        break;
                    }
                }
            }
        }
        return node.negated ? !found : found;
    }

    bool dotMatches(uint32_t inputCode) const {
        return pattern_.flags.dotAll || !isLineTerminator(inputCode);
    }

    bool singleCharMatches(const Node& node, uint32_t inputCode) const {
        switch (node.kind) {
            case NodeKind::Char: return charMatches(node.ch, inputCode);
            case NodeKind::Class: return classMatches(node, inputCode);
            case NodeKind::Dot: return dotMatches(inputCode);
            default: return false;
        }
    }

    // The two decodes, in the matcher's own terms. Both take a position that is
    // already known to be in range, which is the caller's test either way.
    CodePointStep characterAt(size_t pos) const { return codePointAt(input_, pos, unicode_); }
    CodePointStep characterBefore(size_t pos) const {
        return codePointBefore(input_, pos, unicode_);
    }

    // 22.2.2.6 IsWordChar reads a CHARACTER under `u`, and this reads a code
    // UNIT in both modes — but the argument for that is no longer the one it
    // used to be. "WordCharacters is the basic sixty-three" stopped being true
    // the moment `u` and `i` could both be set: 22.2.2.7.1 step 3 grows the
    // set there, so the reading has to survive a bigger one.
    //
    // It does, because of WHICH characters it grows by. Simple case folding
    // sends exactly U+017F and U+212A into the basic sixty-three, and both are
    // in the BMP and neither is a surrogate. So no astral character is a word
    // character, no half of a surrogate pair is one, and a unit read and a
    // character read agree at every position — where a decode would cost a
    // branch per `\b` to reach the same answer.
    //
    // That is a property of the generated fold table rather than of this file,
    // so `tests/regex` walks the whole code space to hold it, and the
    // generator refuses to emit a table that breaks it.
    bool isWordAt(int64_t at) const {
        if (at < 0 || at >= static_cast<int64_t>(input_.size())) return false;
        return rangesContain(wordRanges(ignoreCase_, unicode_),
                             input_[static_cast<size_t>(at)]);
    }

    bool assertionHolds(const Node& node, size_t pos) const {
        switch (node.assertion) {
            case AssertionKind::Start:
                if (pos == 0) return true;
                return pattern_.flags.multiline && isLineTerminator(input_[pos - 1]);
            case AssertionKind::End:
                if (pos == input_.size()) return true;
                return pattern_.flags.multiline && isLineTerminator(input_[pos]);
            case AssertionKind::WordBoundary:
                return isWordAt(static_cast<int64_t>(pos) - 1) !=
                       isWordAt(static_cast<int64_t>(pos));
            case AssertionKind::NotWordBoundary:
                return isWordAt(static_cast<int64_t>(pos) - 1) ==
                       isWordAt(static_cast<int64_t>(pos));
        }
        return false;
    }

    // ---- the continuation ---------------------------------------------------

    bool runCont(const Cont* k, size_t pos) {
        if (failed_) return false;
        if (++steps_ > kStepBudget) {
            return giveUp("this regular expression exceeded bronze's backtracking budget of " +
                          std::to_string(kStepBudget) + " steps");
        }
        switch (k->kind) {
            case Cont::Kind::Done:
                endPos_ = pos;
                return true;
            case Cont::Kind::Sequence:
                return matchSequence(*k->terms, k->index, pos, k->next, k->backward);
            case Cont::Kind::Repeat: {
                // 22.2.2.5.1 step 2.a: a turn that consumed nothing, with the
                // minimum already met, ends the loop rather than repeating for
                // ever. This is what makes `/(a*)*/ ` terminate. "Consumed
                // nothing" is `pos == mark` whichever way the index was
                // moving, so the guard needs no direction of its own.
                if (k->min == 0 && pos == k->mark) return false;
                const uint32_t min2 = k->min == 0 ? 0 : k->min - 1;
                const uint32_t max2 = k->max == kUnbounded ? kUnbounded : k->max - 1;
                return matchRepeat(*k->repeat, min2, max2, pos, k->next, k->backward);
            }
            case Cont::Kind::Capture: {
                const size_t slot = static_cast<size_t>(k->captureIndex) * 2;
                const int64_t oldStart = captures_[slot];
                const int64_t oldEnd = captures_[slot + 1];
                // 22.2.2.8: the group's range is (xe, ye) forward and (ye, xe)
                // backward. `mark` is where the group was ENTERED, which is its
                // END when the direction is backward, so the pair is ordered
                // here rather than assigned in source order.
                const size_t lo = k->mark < pos ? k->mark : pos;
                const size_t hi = k->mark < pos ? pos : k->mark;
                captures_[slot] = static_cast<int64_t>(lo);
                captures_[slot + 1] = static_cast<int64_t>(hi);
                if (runCont(k->next, pos)) return true;
                captures_[slot] = oldStart;
                captures_[slot + 1] = oldEnd;
                return false;
            }
        }
        return false;
    }

    // 22.2.2.4: an Alternative's terms are matched in source order forward and
    // in REVERSE order backward. `index` counts terms already matched either
    // way, so the termination test is the same and only the pick changes.
    bool matchSequence(const std::vector<NodePtr>& terms, size_t index, size_t pos,
                       const Cont* k, bool backward) {
        if (index == terms.size()) return runCont(k, pos);
        Cont next;
        next.kind = Cont::Kind::Sequence;
        next.terms = &terms;
        next.index = index + 1;
        next.backward = backward;
        next.next = k;
        const size_t at = backward ? terms.size() - 1 - index : index;
        return matchNode(*terms[at], pos, &next, backward);
    }

    // ---- the quantifier -----------------------------------------------------

    void clearRepeatCaptures(const Node& node) {
        for (uint32_t i = 0; i < node.captureCount; ++i) {
            const size_t slot = static_cast<size_t>(node.firstCapture + i) * 2;
            captures_[slot] = MatchResult::kUnset;
            captures_[slot + 1] = MatchResult::kUnset;
        }
    }

    void saveRepeatCaptures(const Node& node, std::vector<int64_t>& into) {
        into.clear();
        for (uint32_t i = 0; i < node.captureCount; ++i) {
            const size_t slot = static_cast<size_t>(node.firstCapture + i) * 2;
            into.push_back(captures_[slot]);
            into.push_back(captures_[slot + 1]);
        }
    }

    void restoreRepeatCaptures(const Node& node, const std::vector<int64_t>& from) {
        for (uint32_t i = 0; i < node.captureCount; ++i) {
            const size_t slot = static_cast<size_t>(node.firstCapture + i) * 2;
            captures_[slot] = from[i * 2];
            captures_[slot + 1] = from[i * 2 + 1];
        }
    }

    // A quantified atom that consumes exactly one character and captures
    // nothing. The repetition is then a COUNT, so the whole loop is a scan and
    // the backtracking is a descending (or ascending, when lazy) walk over the
    // counts — no recursion per iteration, which is the difference between
    // `/.*/` working on a 100 kB string and overflowing the stack on it.
    //
    // Backward it is the same scan run the other way. Without that, `(?<=a*)`
    // would be the one construct that reintroduced a frame per repetition —
    // the regression the forward path exists to prevent.
    //
    // Under `u` a repetition is one or two units, so `n` repetitions do NOT
    // leave the index `n` away from where they started. The walk therefore
    // carries a position and moves it one character per step, from the far end
    // of the scan backwards for a greedy quantifier and from the start forwards
    // for a lazy one — still constant work per backtrack, and still no
    // allocation, where remembering every boundary would cost one per scan.
    bool matchSimpleRepeat(const Node& node, size_t pos, const Cont* k, bool backward) {
        const Node& atom = *node.children[0];
        size_t count = 0;
        size_t at = pos;
        while (node.max == kUnbounded || count < node.max) {
            if (backward) {
                if (at == 0) break;
                const CodePointStep step = characterBefore(at);
                if (!singleCharMatches(atom, step.code)) break;
                at -= step.width;
            } else {
                if (at >= input_.size()) break;
                const CodePointStep step = characterAt(at);
                if (!singleCharMatches(atom, step.code)) break;
                at += step.width;
            }
            ++count;
        }
        if (count < node.min) return false;

        // One repetition given back (`shrink`) or taken (`grow`), as an index
        // move. Both only ever run over boundaries the scan above established,
        // so the decode they do is the same one that produced them.
        const auto shrink = [&](size_t cur) {
            return backward ? cur + characterAt(cur).width : cur - characterBefore(cur).width;
        };
        const auto grow = [&](size_t cur) {
            return backward ? cur - characterBefore(cur).width : cur + characterAt(cur).width;
        };

        if (node.greedy) {
            size_t cur = at;
            for (size_t n = count;; --n) {
                if (runCont(k, cur)) return true;
                if (failed_ || n == node.min) return false;
                cur = shrink(cur);
            }
        }
        size_t cur = pos;
        for (uint32_t n = 0; n < node.min; ++n) cur = grow(cur);
        for (size_t n = node.min; n <= count; ++n) {
            if (runCont(k, cur)) return true;
            if (failed_) return false;
            if (n < count) cur = grow(cur);
        }
        return false;
    }

    // 22.2.2.5.1 RepeatMatcher, with `xr` — the state whose inner captures are
    // cleared — spelled as a save/clear/restore around the atom. The clearing
    // is what makes `/(?:(a)|b)*/.exec("ab")[1]` undefined: the second turn
    // round the loop must not inherit the first turn's capture.
    bool matchRepeat(const Node& node, uint32_t min, uint32_t max, size_t pos, const Cont* k,
                     bool backward) {
        if (max == 0) return runCont(k, pos);

        Cont again;
        again.kind = Cont::Kind::Repeat;
        again.repeat = &node;
        again.min = min;
        again.max = max;
        again.mark = pos;
        again.backward = backward;
        again.next = k;

        const Node& atom = *node.children[0];
        std::vector<int64_t> saved;
        if (node.captureCount != 0) {
            saveRepeatCaptures(node, saved);
            clearRepeatCaptures(node);
        }

        if (min != 0) {
            if (matchNode(atom, pos, &again, backward)) return true;
            if (node.captureCount != 0) restoreRepeatCaptures(node, saved);
            return false;
        }
        if (!node.greedy) {
            // Steps 5: the continuation runs against the state BEFORE the
            // clearing, because a lazy quantifier that takes zero turns must
            // leave the captures it found on an earlier one alone.
            if (node.captureCount != 0) restoreRepeatCaptures(node, saved);
            if (runCont(k, pos)) return true;
            if (failed_) return false;
            if (node.captureCount != 0) clearRepeatCaptures(node);
            if (matchNode(atom, pos, &again, backward)) return true;
            if (node.captureCount != 0) restoreRepeatCaptures(node, saved);
            return false;
        }
        if (matchNode(atom, pos, &again, backward)) return true;
        if (node.captureCount != 0) restoreRepeatCaptures(node, saved);
        if (failed_) return false;
        return runCont(k, pos);
    }

    // ---- the tree -----------------------------------------------------------

    bool matchBackreference(const Node& node, size_t pos, const Cont* k, bool backward) {
        const size_t slot = static_cast<size_t>(node.backreference) * 2;
        const int64_t start = captures_[slot];
        const int64_t end = captures_[slot + 1];
        // A group that never participated matches the empty string (22.2.2.9
        // BackreferenceMatcher step 2), which is NOT the same as failing:
        // `/(a)?\1b/.test("b")` is true.
        if (start == MatchResult::kUnset || end == MatchResult::kUnset) return runCont(k, pos);
        const size_t length = static_cast<size_t>(end - start);
        // Backward the captured text ENDS at `pos`, so the comparison begins
        // `length` units earlier and the continuation resumes there.
        size_t from = pos;
        if (backward) {
            if (pos < length) return false;
            from = pos - length;
        } else if (pos + length > input_.size()) {
            return false;
        }
        // Compared CHARACTER by character, which under `u` and `i` is not the
        // same as unit by unit: 282 astral code points have a simple case
        // folding, and a surrogate has none, so a unit-wise fold would compare
        // the halves of `𐐀` against those of `𐐨` unchanged and answer that a
        // backreference does not match text it does. Without `u` the decode is
        // a unit anyway, so this is one loop and not two.
        size_t i = 0;
        while (i < length) {
            const CodePointStep a = codePointAt(input_, static_cast<size_t>(start) + i, unicode_);
            const CodePointStep b = codePointAt(input_, from + i, unicode_);
            if (a.code != b.code) {
                // A character and its fold are always the same width — nothing
                // folds across the BMP boundary — so a width disagreement is a
                // pair that cannot be equal under any canonicalization.
                if (a.width != b.width) return false;
                if (ignoreCase_ && !unicode_ &&
                    (isUnknownCasedUnit(a.code) || isUnknownCasedUnit(b.code))) {
                    // Both sides of a backreference are INPUT, so neither
                    // passed the parser's case-table check. Answering
                    // "different" here would be a guess about a case pair
                    // bronze cannot see — and only the uppercase table has
                    // pairs it cannot see.
                    return giveUp("unsupported: a case-insensitive backreference compared "
                                  "characters bronze has no case table for without the `u` "
                                  "flag (only ASCII, Latin-1, Latin Extended-A, Greek, "
                                  "Cyrillic and Armenian fold under `i` alone)");
                }
                if (canonical(a.code) != canonical(b.code)) return false;
            }
            i += a.width;
        }
        return runCont(k, backward ? pos - length : pos + length);
    }

    // 22.2.2.3 and 22.2.2.6: the two are one matcher because they differ only
    // in the direction the body runs. Neither consumes anything, so the
    // continuation resumes at exactly `pos` either way — and it resumes in the
    // ENCLOSING direction, which the continuation already carries.
    bool matchLookaround(const Node& node, size_t pos, const Cont* k) {
        const bool bodyBackward = node.kind == NodeKind::Lookbehind;
        const std::vector<int64_t> saved = captures_;
        const size_t savedEnd = endPos_;
        Cont done;
        done.kind = Cont::Kind::Done;
        const bool inner = matchNode(*node.children[0], pos, &done, bodyBackward);
        endPos_ = savedEnd;
        if (failed_) return false;

        if (node.lookaroundNegative) {
            // 22.2.2.3: a negative lookaround discards whatever its body
            // captured, whether or not it matched.
            captures_ = saved;
            if (inner) return false;
            return runCont(k, pos);
        }
        if (!inner) {
            captures_ = saved;
            return false;
        }
        // Atomic: the body is matched once and never retried to satisfy what
        // follows, because the spec's continuation for a lookahead accepts
        // immediately. So a failure after this point fails the whole term.
        if (runCont(k, pos)) return true;
        captures_ = saved;
        return false;
    }

    bool matchNode(const Node& node, size_t pos, const Cont* k, bool backward) {
        if (failed_) return false;
        if (++depth_ > kMaxDepth) {
            --depth_;
            return giveUp("this regular expression needed more than " +
                          std::to_string(kMaxDepth) +
                          " levels of backtracking state, which is bronze's limit");
        }
        const bool result = matchNodeInner(node, pos, k, backward);
        --depth_;
        return result;
    }

    // The one character an atom consumes, on the side of `pos` the direction
    // picks: forward reads the character starting at `pos` and resumes after
    // it, backward reads the character ENDING at `pos` and resumes before it.
    // Char, Class and Dot differ only in the test, which `singleCharMatches`
    // already owns — and the two modes differ only in how wide the character
    // is, which the two decodes own. Both facts live here and in no third
    // place.
    bool matchOneCharacter(const Node& node, size_t pos, const Cont* k, bool backward) {
        if (backward) {
            if (pos == 0) return false;
            const CodePointStep step = characterBefore(pos);
            if (!singleCharMatches(node, step.code)) return false;
            return runCont(k, pos - step.width);
        }
        if (pos >= input_.size()) return false;
        const CodePointStep step = characterAt(pos);
        if (!singleCharMatches(node, step.code)) return false;
        return runCont(k, pos + step.width);
    }

    // 22.2.2.7.1's answer for a CharSet whose members are not all characters:
    // the members are tried LONGEST FIRST, each as a whole alternative, and one
    // that matches but whose continuation fails is GIVEN BACK. `/^[\q{abc|a}]bc$/v`
    // matches "abc" only because of that — "abc" is tried, `bc$` fails at the end
    // of the input, and the shorter member is tried in its place.
    //
    // That backtracking is the whole reason this is not folded into
    // `matchOneCharacter`: a class of characters has one way to match at a
    // position, and this has as many ways as it has members.
    bool matchClassSet(const Node& node, size_t pos, const Cont* k, bool backward) {
        for (const std::vector<uint32_t>& member : node.strings) {
            size_t at = pos;
            bool consumed = true;
            for (size_t i = 0; i < member.size() && consumed; ++i) {
                if (backward) {
                    // Read right to left, so the member is consumed from its END:
                    // its last character is the one nearest `pos`.
                    if (at == 0) { consumed = false; break; }
                    const CodePointStep step = characterBefore(at);
                    if (step.code != member[member.size() - 1 - i]) { consumed = false; break; }
                    at -= step.width;
                } else {
                    if (at >= input_.size()) { consumed = false; break; }
                    const CodePointStep step = characterAt(at);
                    if (step.code != member[i]) { consumed = false; break; }
                    at += step.width;
                }
            }
            if (!consumed) continue;
            if (runCont(k, at)) return true;
            if (failed_) return false;
        }
        // The one-character members next, and the zero-length one last: the same
        // descending order by length, continued past where `strings` stops.
        if (!node.ranges.empty()) {
            if (matchOneCharacter(node, pos, k, backward)) return true;
            if (failed_) return false;
        }
        if (node.matchesEmpty) return runCont(k, pos);
        return false;
    }

    bool matchNodeInner(const Node& node, size_t pos, const Cont* k, bool backward) {
        switch (node.kind) {
            case NodeKind::Alternation:
                for (const NodePtr& alt : node.children) {
                    if (matchSequence(alt->children, 0, pos, k, backward)) return true;
                    if (failed_) return false;
                }
                return false;
            case NodeKind::Sequence:
                return matchSequence(node.children, 0, pos, k, backward);
            case NodeKind::Class:
                if (!node.strings.empty() || node.matchesEmpty) {
                    return matchClassSet(node, pos, k, backward);
                }
                return matchOneCharacter(node, pos, k, backward);
            case NodeKind::Char:
            case NodeKind::Dot:
                return matchOneCharacter(node, pos, k, backward);
            case NodeKind::Assertion:
                // An assertion consumes nothing whichever direction it ran, and
                // `^`, `$` and `\b` all ask about the units either side of
                // `pos` — so there is no backward form of one.
                if (!assertionHolds(node, pos)) return false;
                return runCont(k, pos);
            case NodeKind::Lookahead:
            case NodeKind::Lookbehind:
                return matchLookaround(node, pos, k);
            case NodeKind::Group: {
                if (node.captureIndex == 0) {
                    return matchNode(*node.children[0], pos, k, backward);
                }
                Cont close;
                close.kind = Cont::Kind::Capture;
                close.captureIndex = node.captureIndex;
                close.mark = pos;
                close.next = k;
                return matchNode(*node.children[0], pos, &close, backward);
            }
            case NodeKind::Backreference:
                return matchBackreference(node, pos, k, backward);
            case NodeKind::Repeat:
                if (node.simpleAtom) return matchSimpleRepeat(node, pos, k, backward);
                return matchRepeat(node, node.min, node.max, pos, k, backward);
        }
        return false;
    }
};

}  // namespace

ExecStatus matchAt(const Pattern& pattern, UnitsView input, size_t at, MatchResult& out,
                   std::string& error) {
    if (at > input.size()) return ExecStatus::NoMatch;
    Matcher matcher(pattern, input);
    return matcher.run(at, out, error);
}

ExecStatus search(const Pattern& pattern, UnitsView input, size_t from, MatchResult& out,
                  std::string& error) {
    if (from > input.size()) return ExecStatus::NoMatch;
    Matcher matcher(pattern, input);
    // The scan is the spec's own: 22.2.7.2 step 12 advances the start index by
    // AdvanceStringIndex and tries again, which is why a pattern anchored with
    // `^` and no `m` still costs a walk over the string. `<= size()` because a
    // pattern can match the empty string at the very end.
    //
    // Under `u` that step is a CHARACTER, so no attempt ever begins between the
    // halves of a surrogate pair: `/\uDE00/u` does not find the trailing half of
    // an astral character, because index 1 of a two-unit character is not a
    // position the scan visits.
    const bool unicode = pattern.flags.unicodeMode();
    for (size_t at = from; at <= input.size(); at = advanceStringIndex(input, at, unicode)) {
        const ExecStatus status = matcher.run(at, out, error);
        if (status != ExecStatus::NoMatch) return status;
    }
    return ExecStatus::NoMatch;
}

}  // namespace bronze::regex
