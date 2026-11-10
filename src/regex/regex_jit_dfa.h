#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "regex/chars.h"
#include "regex/pattern.h"
#include "regex/regex_jit.h"
#include "regex/unicode.h"

namespace bronze::regex::jit_dfa {

inline constexpr uint32_t kUnbounded = 0xFFFFFFFFu;
inline constexpr uint32_t kMaxBoundedExpansion = 32;
inline constexpr int32_t kMaxFixedWidth = 1000000;

inline bool isLineTerminatorAscii(uint32_t u) noexcept {
    return u == 0x000A || u == 0x000D;
}

// Builds a 128-bit ASCII membership mask (two uint64_t words for [0..63] and [64..127])
// matching the exact semantics of `Matcher::singleCharMatches` in `matcher.cpp`.
struct AsciiMask {
    uint64_t lo = 0;
    uint64_t hi = 0;

    void set(uint32_t c) noexcept {
        if (c < 64) {
            lo |= (uint64_t{1} << c);
        } else if (c < 128) {
            hi |= (uint64_t{1} << (c - 64));
        }
    }

    bool test(uint32_t c) const noexcept {
        if (c < 64) return ((lo >> c) & 1u) != 0;
        if (c < 128) return ((hi >> (c - 64)) & 1u) != 0;
        return false;
    }

    bool empty() const noexcept { return lo == 0 && hi == 0; }
};

inline bool charMatchesAscii(uint32_t patternCode, uint32_t inputCode, const Flags& flags) {
    return canonicalize(inputCode, flags.ignoreCase, flags.unicodeMode()) == patternCode;
}

inline bool classMatchesAscii(const Node& node, uint32_t inputCode, const Flags& flags) {
    bool found = rangesContain(node.ranges, inputCode);
    if (!found && flags.ignoreCase) {
        const uint32_t cc = canonicalize(inputCode, flags.ignoreCase, flags.unicodeMode());
        found = rangesContain(node.ranges, cc);
        if (!found) {
            for (uint32_t candidate : caseCandidates(cc, flags.unicodeMode())) {
                if (rangesContain(node.ranges, candidate)) {
                    found = true;
                    break;
                }
            }
        }
    }
    return node.negated ? !found : found;
}

inline bool dotMatchesAscii(uint32_t inputCode, const Flags& flags) noexcept {
    return flags.dotAll || !isLineTerminatorAscii(inputCode);
}

inline AsciiMask computeAsciiMask(const Node& node, const Flags& flags) {
    AsciiMask mask;
    for (uint32_t c = 0; c < 128; ++c) {
        bool ok = false;
        switch (node.kind) {
            case NodeKind::Char:
                ok = charMatchesAscii(node.ch, c, flags);
                break;
            case NodeKind::Class:
                ok = classMatchesAscii(node, c, flags);
                break;
            case NodeKind::Dot:
                ok = dotMatchesAscii(c, flags);
                break;
            default:
                break;
        }
        if (ok) mask.set(c);
    }
    return mask;
}

// Checks whether a sub-AST can match the empty string (nullable).
// Repeat(min=0) over a nullable atom in JS has an empty-iteration guard that is
// context-sensitive unless it is a simple non-nullable atom, so we reject
// quantified nullable subtrees from the DFA fast path and let the interpreter run them.
inline bool canMatchEmpty(const Node& node) {
    switch (node.kind) {
        case NodeKind::Char:
        case NodeKind::Dot:
            return false;
        case NodeKind::Class:
            return node.matchesEmpty;
        case NodeKind::Group:
            return node.children.empty() || canMatchEmpty(*node.children[0]);
        case NodeKind::Sequence:
            for (const NodePtr& child : node.children) {
                if (!canMatchEmpty(*child)) return false;
            }
            return true;
        case NodeKind::Alternation:
            for (const NodePtr& child : node.children) {
                if (canMatchEmpty(*child)) return true;
            }
            return false;
        case NodeKind::Repeat:
            if (node.min == 0) return true;
            return node.children.empty() || canMatchEmpty(*node.children[0]);
        default:
            return true;
    }
}

// Thompson NFA with ordered epsilon transitions (PikeVM priority order).
struct NfaInst {
    enum class Kind : uint8_t {
        Match,
        ByteSet,
        Split,
        Jump,
    };

    Kind kind = Kind::Match;
    AsciiMask mask{};
    uint32_t out0 = 0;
    uint32_t out1 = 0;
};

class NfaCompiler {
public:
    explicit NfaCompiler(const Flags& flags) : flags_(flags) {}

    bool compileRoot(const Node& root, bool& out_anchors_at_start) {
        out_anchors_at_start = false;
        const Node* body = &root;
        std::vector<const Node*> seq_terms;

        // Support leading `^` (when !multiline) at the start of a top-level Sequence.
        if (root.kind == NodeKind::Assertion) {
            if (root.assertion == AssertionKind::Start && !flags_.multiline) {
                out_anchors_at_start = true;
                emitMatch();
                start_nfa_ = 0;
                return true;
            }
            return false;
        }

        if (root.kind == NodeKind::Sequence && !root.children.empty()) {
            const Node& first = *root.children[0];
            if (first.kind == NodeKind::Assertion &&
                first.assertion == AssertionKind::Start &&
                !flags_.multiline) {
                out_anchors_at_start = true;
                for (size_t i = 1; i < root.children.size(); ++i) {
                    seq_terms.push_back(root.children[i].get());
                }
                const uint32_t match_id = emitMatch();
                uint32_t next = match_id;
                for (size_t i = seq_terms.size(); i-- > 0;) {
                    next = compileNode(*seq_terms[i], next);
                    if (next == kInvalid) return false;
                }
                start_nfa_ = next;
                return true;
            }
        }

        const uint32_t match_id = emitMatch();
        const uint32_t start = compileNode(*body, match_id);
        if (start == kInvalid) return false;
        start_nfa_ = start;
        return true;
    }

    uint32_t startState() const noexcept { return start_nfa_; }
    const std::vector<NfaInst>& insts() const noexcept { return insts_; }

private:
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    const Flags& flags_;
    std::vector<NfaInst> insts_;
    uint32_t start_nfa_ = 0;

    uint32_t emit(const NfaInst& inst) {
        if (insts_.size() >= RegexJitPlan::kMaxNfaStates) return kInvalid;
        insts_.push_back(inst);
        return static_cast<uint32_t>(insts_.size() - 1);
    }

    uint32_t emitMatch() {
        return emit(NfaInst{NfaInst::Kind::Match, {}, 0, 0});
    }

    uint32_t emitByteSet(const AsciiMask& mask, uint32_t next) {
        return emit(NfaInst{NfaInst::Kind::ByteSet, mask, next, 0});
    }

    uint32_t emitSplit(uint32_t first, uint32_t second) {
        return emit(NfaInst{NfaInst::Kind::Split, {}, first, second});
    }

    uint32_t compileNode(const Node& node, uint32_t next) {
        if (next == kInvalid) return kInvalid;
        switch (node.kind) {
            case NodeKind::Char:
            case NodeKind::Dot: {
                return emitByteSet(computeAsciiMask(node, flags_), next);
            }
            case NodeKind::Class: {
                if (!node.strings.empty() || node.matchesEmpty) return kInvalid;
                return emitByteSet(computeAsciiMask(node, flags_), next);
            }
            case NodeKind::Group: {
                if (node.children.empty()) return kInvalid;
                return compileNode(*node.children[0], next);
            }
            case NodeKind::Sequence: {
                uint32_t cur = next;
                for (size_t i = node.children.size(); i-- > 0;) {
                    cur = compileNode(*node.children[i], cur);
                    if (cur == kInvalid) return kInvalid;
                }
                return cur;
            }
            case NodeKind::Alternation: {
                if (node.children.empty()) return next;
                // Build right-to-left chain of ordered Splits so left alternatives have higher priority.
                uint32_t cur = compileNode(*node.children.back(), next);
                if (cur == kInvalid) return kInvalid;
                for (size_t i = node.children.size() - 1; i-- > 0;) {
                    uint32_t left = compileNode(*node.children[i], next);
                    if (left == kInvalid) return kInvalid;
                    cur = emitSplit(left, cur);
                    if (cur == kInvalid) return kInvalid;
                }
                return cur;
            }
            case NodeKind::Repeat: {
                if (node.children.empty()) return kInvalid;
                const Node& atom = *node.children[0];
                // Reject repetitions over nullable atoms so empty-match loop semantics stay in interpreter.
                if (canMatchEmpty(atom)) return kInvalid;
                if (node.min > kMaxBoundedExpansion) return kInvalid;
                if (node.max != kUnbounded && node.max > kMaxBoundedExpansion) return kInvalid;

                uint32_t cur = next;
                if (node.max == kUnbounded) {
                    const uint32_t split_id = emitSplit(0, 0);
                    if (split_id == kInvalid) return kInvalid;
                    const uint32_t body = compileNode(atom, split_id);
                    if (body == kInvalid) return kInvalid;
                    if (node.greedy) {
                        insts_[split_id].out0 = body;
                        insts_[split_id].out1 = next;
                    } else {
                        insts_[split_id].out0 = next;
                        insts_[split_id].out1 = body;
                    }
                    cur = split_id;
                } else {
                    for (uint32_t k = node.min; k < node.max; ++k) {
                        const uint32_t split_id = emitSplit(0, 0);
                        if (split_id == kInvalid) return kInvalid;
                        const uint32_t body = compileNode(atom, cur);
                        if (body == kInvalid) return kInvalid;
                        if (node.greedy) {
                            insts_[split_id].out0 = body;
                            insts_[split_id].out1 = next;
                        } else {
                            insts_[split_id].out0 = next;
                            insts_[split_id].out1 = body;
                        }
                        cur = split_id;
                    }
                }
                for (uint32_t k = 0; k < node.min; ++k) {
                    cur = compileNode(atom, cur);
                    if (cur == kInvalid) return kInvalid;
                }
                return cur;
            }
            case NodeKind::Assertion:
            case NodeKind::Lookahead:
            case NodeKind::Lookbehind:
            case NodeKind::Backreference:
                return kInvalid;
        }
        return kInvalid;
    }
};

// Ordered subset construction (PikeVM priority simulation) from NFA to DFA.
// Each DFA state is an ordered list of `ByteSet` or `Match` NFA instruction IDs,
// truncated immediately after the first `Match` instruction (since higher-priority
// threads shadow lower-priority threads once they accept).
struct DfaStateKey {
    std::vector<uint32_t> nfa_list;
    bool operator==(const DfaStateKey& other) const noexcept {
        return nfa_list == other.nfa_list;
    }
};

struct DfaStateKeyHash {
    size_t operator()(const DfaStateKey& k) const noexcept {
        size_t h = 1469598103934665603ULL;
        for (uint32_t v : k.nfa_list) {
            h ^= v;
            h *= 1099511628211ULL;
        }
        return h;
    }
};

class DfaBuilder {
public:
    explicit DfaBuilder(const std::vector<NfaInst>& insts)
        : insts_(insts), visited_(insts.size(), 0) {}

    bool build(
        uint32_t start_nfa,
        std::vector<int32_t>& out_transitions,
        std::vector<int32_t>& out_accept,
        int32_t& out_start_state,
        int32_t& out_num_states) {
        std::vector<uint32_t> initial;
        epsilonClosure({start_nfa}, initial);
        if (initial.empty()) {
            out_start_state = -1;
            out_num_states = 0;
            return true;
        }

        out_start_state = internState(initial);
        if (out_start_state < 0) return false;

        std::vector<uint32_t> next_roots;
        std::vector<uint32_t> next_closure;

        for (size_t s = 0; s < states_.size(); ++s) {
            const std::vector<uint32_t> cur_list = states_[s];

            // Determine accept status of state `s`:
            //   0 = not accepting
            //   1 = Match is in cur_list, and at least one higher-priority ByteSet precedes it
            //   2 = Match is the very first item in cur_list (highest priority thread accepted; stop immediately!)
            int32_t acc = 0;
            for (size_t idx = 0; idx < cur_list.size(); ++idx) {
                if (insts_[cur_list[idx]].kind == NfaInst::Kind::Match) {
                    acc = (idx == 0) ? 2 : 1;
                    break;
                }
            }
            out_accept.push_back(acc);

            // If acc == 2, the highest-priority thread has already matched at this position,
            // so no further bytes need to be consumed from this state.
            if (acc == 2) {
                for (uint32_t b = 0; b < 256; ++b) {
                    out_transitions[s * 256 + b] = -1;
                }
                continue;
            }

            // Compute transitions for ASCII bytes [0..127]; bytes [128..255] go to -1.
            for (uint32_t b = 0; b < 128; ++b) {
                next_roots.clear();
                for (uint32_t id : cur_list) {
                    const NfaInst& in = insts_[id];
                    if (in.kind == NfaInst::Kind::Match) {
                        // Lower-priority threads after Match were already pruned during closure,
                        // and any thread that reaches Match does not consume more bytes.
                        break;
                    }
                    if (in.kind == NfaInst::Kind::ByteSet && in.mask.test(b)) {
                        next_roots.push_back(in.out0);
                    }
                }
                if (next_roots.empty()) {
                    out_transitions[s * 256 + b] = -1;
                    continue;
                }
                epsilonClosure(next_roots, next_closure);
                if (next_closure.empty()) {
                    out_transitions[s * 256 + b] = -1;
                    continue;
                }
                int32_t ns = internState(next_closure);
                if (ns < 0) return false;
                // Ensure `out_transitions` is resized when `internState` adds a state.
                if (out_transitions.size() < states_.size() * 256) {
                    out_transitions.resize(states_.size() * 256, -1);
                }
                out_transitions[s * 256 + b] = ns;
            }
            for (uint32_t b = 128; b < 256; ++b) {
                out_transitions[s * 256 + b] = -1;
            }
        }

        out_num_states = static_cast<int32_t>(states_.size());
        return true;
    }

private:
    const std::vector<NfaInst>& insts_;
    std::vector<uint32_t> visited_;
    uint32_t gen_ = 0;
    std::vector<std::vector<uint32_t>> states_;
    std::unordered_map<DfaStateKey, int32_t, DfaStateKeyHash> state_map_;

    void epsilonClosure(const std::vector<uint32_t>& roots, std::vector<uint32_t>& out) {
        out.clear();
        if (++gen_ == 0) {
            std::fill(visited_.begin(), visited_.end(), 0);
            gen_ = 1;
        }
        std::vector<uint32_t> stack;
        for (size_t i = roots.size(); i-- > 0;) {
            stack.push_back(roots[i]);
        }
        while (!stack.empty()) {
            const uint32_t id = stack.back();
            stack.pop_back();
            if (visited_[id] == gen_) continue;
            visited_[id] = gen_;

            const NfaInst& in = insts_[id];
            switch (in.kind) {
                case NfaInst::Kind::Match:
                    out.push_back(id);
                    // Priority cut: in PikeVM/backtracking semantics, once a higher-priority
                    // path reaches Match at the current index, all lower-priority paths at the
                    // same index are shadowed and can never win.
                    return;
                case NfaInst::Kind::ByteSet:
                    out.push_back(id);
                    break;
                case NfaInst::Kind::Split:
                    stack.push_back(in.out1);
                    stack.push_back(in.out0);
                    break;
                case NfaInst::Kind::Jump:
                    stack.push_back(in.out0);
                    break;
            }
        }
    }

    int32_t internState(const std::vector<uint32_t>& nfa_list) {
        DfaStateKey key{nfa_list};
        auto it = state_map_.find(key);
        if (it != state_map_.end()) return it->second;
        if (states_.size() >= RegexJitPlan::kMaxDfaStates) return -1;
        const int32_t id = static_cast<int32_t>(states_.size());
        states_.push_back(nfa_list);
        state_map_.emplace(std::move(key), id);
        return id;
    }
};

// Returns the exact number of ASCII code units consumed by `node` on every DFA
// match, or `-1` if `node` can match variable-length spans. Because the JIT DFA
// bails out on any code unit >= 128, every single-unit atom consumes 1 unit.
inline int32_t fixedAsciiWidth(const Node& node) {
    switch (node.kind) {
        case NodeKind::Char:
        case NodeKind::Dot:
            return 1;
        case NodeKind::Class:
            if (!node.strings.empty() || node.matchesEmpty) return -1;
            return 1;
        case NodeKind::Assertion:
            return 0;
        case NodeKind::Group:
            return node.children.empty() ? 0 : fixedAsciiWidth(*node.children[0]);
        case NodeKind::Sequence: {
            int64_t total = 0;
            for (const NodePtr& child : node.children) {
                const int32_t w = fixedAsciiWidth(*child);
                if (w < 0) return -1;
                total += w;
                if (total > kMaxFixedWidth) return -1;
            }
            return static_cast<int32_t>(total);
        }
        case NodeKind::Alternation: {
            if (node.children.empty()) return 0;
            const int32_t w0 = fixedAsciiWidth(*node.children[0]);
            if (w0 < 0) return -1;
            for (size_t i = 1; i < node.children.size(); ++i) {
                if (fixedAsciiWidth(*node.children[i]) != w0) return -1;
            }
            return w0;
        }
        case NodeKind::Repeat: {
            if (node.min != node.max || node.max == kUnbounded) return -1;
            if (node.min == 0 || node.children.empty()) return 0;
            const int32_t w = fixedAsciiWidth(*node.children[0]);
            if (w < 0) return -1;
            const uint64_t total = static_cast<uint64_t>(w) * node.min;
            if (total > static_cast<uint64_t>(kMaxFixedWidth)) return -1;
            return static_cast<int32_t>(total);
        }
        default:
            return -1;
    }
}

inline bool subtreeHasCaptures(const Node& node) {
    if (node.kind == NodeKind::Group && node.captureIndex > 0) return true;
    for (const NodePtr& child : node.children) {
        if (subtreeHasCaptures(*child)) return true;
    }
    return false;
}

inline bool resolveFixedCapturesRec(
    const Node& node,
    CaptureBoundExpr start,
    CaptureBoundExpr end,
    std::vector<FixedCaptureSpec>& specs,
    std::vector<bool>& seen) {
    if (!subtreeHasCaptures(node)) return true;
    switch (node.kind) {
        case NodeKind::Group: {
            if (node.captureIndex > 0) {
                const size_t idx = node.captureIndex - 1;
                if (idx >= specs.size() || seen[idx]) return false;
                seen[idx] = true;
                specs[idx] = FixedCaptureSpec{start, end};
            }
            return node.children.empty() ||
                   resolveFixedCapturesRec(*node.children[0], start, end, specs, seen);
        }
        case NodeKind::Sequence: {
            size_t l = 0;
            const size_t m = node.children.size();
            CaptureBoundExpr curStart = start;
            while (l < m) {
                const int32_t w = fixedAsciiWidth(*node.children[l]);
                if (w < 0) break;
                const CaptureBoundExpr nextStart{curStart.fromEnd, curStart.delta + w};
                if (!resolveFixedCapturesRec(*node.children[l], curStart, nextStart, specs, seen)) {
                    return false;
                }
                curStart = nextStart;
                ++l;
            }
            if (l == m) return true;

            size_t r = m;
            CaptureBoundExpr curEnd = end;
            while (r > l) {
                const int32_t w = fixedAsciiWidth(*node.children[r - 1]);
                if (w < 0) break;
                const CaptureBoundExpr prevEnd{curEnd.fromEnd, curEnd.delta - w};
                if (!resolveFixedCapturesRec(*node.children[r - 1], prevEnd, curEnd, specs, seen)) {
                    return false;
                }
                curEnd = prevEnd;
                --r;
            }
            if (r - l == 1) {
                return resolveFixedCapturesRec(*node.children[l], curStart, curEnd, specs, seen);
            }
            for (size_t k = l; k < r; ++k) {
                if (subtreeHasCaptures(*node.children[k])) return false;
            }
            return true;
        }
        default:
            // Captures inside Alternation or Repeat may be skipped or overwritten across turns.
            return false;
    }
}

// Resolves deterministic fixed-offset capture bounds for patterns whose capture
// groups always participate and are anchored to `[match_start, match_end]` by
// fixed-width prefixes/suffixes (such as `/(\d+(?:\.\d+)?)px/` or `/(\r\n|\r|\n)/`).
inline bool tryResolveFixedAsciiCaptures(
    const Node& root,
    uint32_t groupCount,
    std::vector<FixedCaptureSpec>& outSpecs) {
    if (groupCount == 0) {
        outSpecs.clear();
        return true;
    }
    std::vector<FixedCaptureSpec> specs(groupCount);
    std::vector<bool> seen(groupCount, false);
    if (!resolveFixedCapturesRec(root, CaptureBoundExpr{false, 0}, CaptureBoundExpr{true, 0},
                                 specs, seen)) {
        return false;
    }
    for (bool s : seen) {
        if (!s) return false;
    }
    outSpecs = std::move(specs);
    return true;
}

}  // namespace bronze::regex::jit_dfa
