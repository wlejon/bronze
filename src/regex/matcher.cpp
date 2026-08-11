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

bool isLineTerminator(uint16_t u) {
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
    const Cont* next = nullptr;
};

class Matcher {
public:
    Matcher(const Pattern& pattern, UnitsView input)
        : pattern_(pattern), input_(input), ignoreCase_(pattern.flags.ignoreCase) {
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
        const bool ok = matchNode(*pattern_.root, at, &done);
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

    // ---- single-unit tests --------------------------------------------------

    bool charMatches(uint16_t patternUnit, uint16_t inputUnit) const {
        // The pattern's unit was canonicalized when it was parsed, and a unit
        // the parser refused to canonicalize can never appear there — so one
        // canonicalization here is the whole comparison.
        return canonicalize(inputUnit, ignoreCase_) == patternUnit;
    }

    // 22.2.2.7.1: found is "the set holds SOME member whose canonicalization is
    // the input's", which is not "the input's canonicalization is in the set".
    // `/[µ]/i` matches U+039C because Canonicalize(U+00B5) is U+039C, and no
    // amount of canonicalizing the input alone finds that.
    bool classMatches(const Node& node, uint16_t inputUnit) const {
        bool found = rangesContain(node.ranges, inputUnit);
        if (!found && ignoreCase_) {
            const uint16_t cc = canonicalize(inputUnit, true);
            found = rangesContain(node.ranges, cc);
            if (!found) {
                for (uint16_t candidate : caseCandidates(cc)) {
                    if (rangesContain(node.ranges, candidate)) {
                        found = true;
                        break;
                    }
                }
            }
        }
        return node.negated ? !found : found;
    }

    bool dotMatches(uint16_t inputUnit) const {
        return pattern_.flags.dotAll || !isLineTerminator(inputUnit);
    }

    bool singleUnitMatches(const Node& node, uint16_t inputUnit) const {
        switch (node.kind) {
            case NodeKind::Char: return charMatches(node.ch, inputUnit);
            case NodeKind::Class: return classMatches(node, inputUnit);
            case NodeKind::Dot: return dotMatches(inputUnit);
            default: return false;
        }
    }

    bool isWordAt(int64_t at) const {
        if (at < 0 || at >= static_cast<int64_t>(input_.size())) return false;
        return rangesContain(wordRanges(ignoreCase_), input_[static_cast<size_t>(at)]);
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
                return matchSequence(*k->terms, k->index, pos, k->next);
            case Cont::Kind::Repeat: {
                // 22.2.2.5.1 step 2.a: a turn that consumed nothing, with the
                // minimum already met, ends the loop rather than repeating for
                // ever. This is what makes `/(a*)*/ ` terminate.
                if (k->min == 0 && pos == k->mark) return false;
                const uint32_t min2 = k->min == 0 ? 0 : k->min - 1;
                const uint32_t max2 = k->max == kUnbounded ? kUnbounded : k->max - 1;
                return matchRepeat(*k->repeat, min2, max2, pos, k->next);
            }
            case Cont::Kind::Capture: {
                const size_t slot = static_cast<size_t>(k->captureIndex) * 2;
                const int64_t oldStart = captures_[slot];
                const int64_t oldEnd = captures_[slot + 1];
                captures_[slot] = static_cast<int64_t>(k->mark);
                captures_[slot + 1] = static_cast<int64_t>(pos);
                if (runCont(k->next, pos)) return true;
                captures_[slot] = oldStart;
                captures_[slot + 1] = oldEnd;
                return false;
            }
        }
        return false;
    }

    bool matchSequence(const std::vector<NodePtr>& terms, size_t index, size_t pos,
                       const Cont* k) {
        if (index == terms.size()) return runCont(k, pos);
        Cont next;
        next.kind = Cont::Kind::Sequence;
        next.terms = &terms;
        next.index = index + 1;
        next.next = k;
        return matchNode(*terms[index], pos, &next);
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

    // A quantified atom that consumes exactly one unit and captures nothing.
    // The repetition is then a COUNT, so the whole loop is a scan and the
    // backtracking is a descending (or ascending, when lazy) walk over the
    // counts — no recursion per iteration, which is the difference between
    // `/.*/` working on a 100 kB string and overflowing the stack on it.
    bool matchSimpleRepeat(const Node& node, size_t pos, const Cont* k) {
        const Node& atom = *node.children[0];
        size_t count = 0;
        size_t at = pos;
        while ((node.max == kUnbounded || count < node.max) && at < input_.size() &&
               singleUnitMatches(atom, input_[at])) {
            ++at;
            ++count;
        }
        if (count < node.min) return false;
        if (node.greedy) {
            for (size_t n = count;; --n) {
                if (runCont(k, pos + n)) return true;
                if (failed_ || n == node.min) return false;
            }
        }
        for (size_t n = node.min; n <= count; ++n) {
            if (runCont(k, pos + n)) return true;
            if (failed_) return false;
        }
        return false;
    }

    // 22.2.2.5.1 RepeatMatcher, with `xr` — the state whose inner captures are
    // cleared — spelled as a save/clear/restore around the atom. The clearing
    // is what makes `/(?:(a)|b)*/.exec("ab")[1]` undefined: the second turn
    // round the loop must not inherit the first turn's capture.
    bool matchRepeat(const Node& node, uint32_t min, uint32_t max, size_t pos, const Cont* k) {
        if (max == 0) return runCont(k, pos);

        Cont again;
        again.kind = Cont::Kind::Repeat;
        again.repeat = &node;
        again.min = min;
        again.max = max;
        again.mark = pos;
        again.next = k;

        const Node& atom = *node.children[0];
        std::vector<int64_t> saved;
        if (node.captureCount != 0) {
            saveRepeatCaptures(node, saved);
            clearRepeatCaptures(node);
        }

        if (min != 0) {
            if (matchNode(atom, pos, &again)) return true;
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
            if (matchNode(atom, pos, &again)) return true;
            if (node.captureCount != 0) restoreRepeatCaptures(node, saved);
            return false;
        }
        if (matchNode(atom, pos, &again)) return true;
        if (node.captureCount != 0) restoreRepeatCaptures(node, saved);
        if (failed_) return false;
        return runCont(k, pos);
    }

    // ---- the tree -----------------------------------------------------------

    bool matchBackreference(const Node& node, size_t pos, const Cont* k) {
        const size_t slot = static_cast<size_t>(node.backreference) * 2;
        const int64_t start = captures_[slot];
        const int64_t end = captures_[slot + 1];
        // A group that never participated matches the empty string (22.2.2.9
        // BackreferenceMatcher step 2), which is NOT the same as failing:
        // `/(a)?\1b/.test("b")` is true.
        if (start == MatchResult::kUnset || end == MatchResult::kUnset) return runCont(k, pos);
        const size_t length = static_cast<size_t>(end - start);
        if (pos + length > input_.size()) return false;
        for (size_t i = 0; i < length; ++i) {
            const uint16_t a = input_[static_cast<size_t>(start) + i];
            const uint16_t b = input_[pos + i];
            if (a == b) continue;
            if (ignoreCase_ && (isUnknownCasedUnit(a) || isUnknownCasedUnit(b))) {
                // Both sides of a backreference are INPUT, so neither passed
                // the parser's case-table check. Answering "different" here
                // would be a guess about a case pair bronze cannot see.
                return giveUp("unsupported: a case-insensitive backreference compared "
                              "characters bronze has no case table for (only ASCII and "
                              "Latin-1 fold under the `i` flag)");
            }
            if (canonicalize(a, ignoreCase_) != canonicalize(b, ignoreCase_)) return false;
        }
        return runCont(k, pos + length);
    }

    bool matchLookahead(const Node& node, size_t pos, const Cont* k) {
        const std::vector<int64_t> saved = captures_;
        const size_t savedEnd = endPos_;
        Cont done;
        done.kind = Cont::Kind::Done;
        const bool inner = matchNode(*node.children[0], pos, &done);
        endPos_ = savedEnd;
        if (failed_) return false;

        if (node.lookaheadNegative) {
            // 22.2.2.3: a negative lookahead discards whatever its body
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

    bool matchNode(const Node& node, size_t pos, const Cont* k) {
        if (failed_) return false;
        if (++depth_ > kMaxDepth) {
            --depth_;
            return giveUp("this regular expression needed more than " +
                          std::to_string(kMaxDepth) +
                          " levels of backtracking state, which is bronze's limit");
        }
        const bool result = matchNodeInner(node, pos, k);
        --depth_;
        return result;
    }

    bool matchNodeInner(const Node& node, size_t pos, const Cont* k) {
        switch (node.kind) {
            case NodeKind::Alternation:
                for (const NodePtr& alt : node.children) {
                    if (matchSequence(alt->children, 0, pos, k)) return true;
                    if (failed_) return false;
                }
                return false;
            case NodeKind::Sequence:
                return matchSequence(node.children, 0, pos, k);
            case NodeKind::Char:
                if (pos >= input_.size() || !charMatches(node.ch, input_[pos])) return false;
                return runCont(k, pos + 1);
            case NodeKind::Class:
                if (pos >= input_.size() || !classMatches(node, input_[pos])) return false;
                return runCont(k, pos + 1);
            case NodeKind::Dot:
                if (pos >= input_.size() || !dotMatches(input_[pos])) return false;
                return runCont(k, pos + 1);
            case NodeKind::Assertion:
                if (!assertionHolds(node, pos)) return false;
                return runCont(k, pos);
            case NodeKind::Lookahead:
                return matchLookahead(node, pos, k);
            case NodeKind::Group: {
                if (node.captureIndex == 0) return matchNode(*node.children[0], pos, k);
                Cont close;
                close.kind = Cont::Kind::Capture;
                close.captureIndex = node.captureIndex;
                close.mark = pos;
                close.next = k;
                return matchNode(*node.children[0], pos, &close);
            }
            case NodeKind::Backreference:
                return matchBackreference(node, pos, k);
            case NodeKind::Repeat:
                if (node.simpleAtom) return matchSimpleRepeat(node, pos, k);
                return matchRepeat(node, node.min, node.max, pos, k);
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
    // one and tries again, which is why a pattern anchored with `^` and no `m`
    // still costs a walk over the string. `<= size()` because a pattern can
    // match the empty string at the very end.
    for (size_t at = from; at <= input.size(); ++at) {
        const ExecStatus status = matcher.run(at, out, error);
        if (status != ExecStatus::NoMatch) return status;
    }
    return ExecStatus::NoMatch;
}

}  // namespace bronze::regex
