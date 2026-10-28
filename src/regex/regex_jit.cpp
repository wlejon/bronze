#include "regex/regex_jit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "regex/chars.h"
#include "regex/unicode.h"

namespace bronze::regex {

namespace {

constexpr uint32_t kUnbounded = 0xFFFFFFFFu;
constexpr uint32_t kMaxBoundedExpansion = 32;

bool isLineTerminatorAscii(uint32_t u) noexcept {
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

bool charMatchesAscii(uint32_t patternCode, uint32_t inputCode, const Flags& flags) {
    return canonicalize(inputCode, flags.ignoreCase, flags.unicodeMode()) == patternCode;
}

bool classMatchesAscii(const Node& node, uint32_t inputCode, const Flags& flags) {
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

bool dotMatchesAscii(uint32_t inputCode, const Flags& flags) noexcept {
    return flags.dotAll || !isLineTerminatorAscii(inputCode);
}

AsciiMask computeAsciiMask(const Node& node, const Flags& flags) {
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
bool canMatchEmpty(const Node& node) {
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

// Loads a 16-bit UTF-16 code unit `units[unit_index]` as `i32` in `[0, 65535]`
// using `GrammarBuilder::load_byte` (little-endian low + high bytes).
brass::Value* emitLoadUtf16Unit(
    brass::codegen::GrammarBuilder& gb,
    brass::Value* units_ptr,
    brass::Value* unit_index) {
    using namespace brass;
    Builder& b = gb.builder();
    Value* byte_off = b.build_shl(unit_index, gb.const_i64(1));
    Value* lo_byte = gb.load_byte(units_ptr, byte_off);
    Value* hi_off = b.build_add(byte_off, gb.const_i64(1));
    Value* hi_byte = gb.load_byte(units_ptr, hi_off);
    Value* hi_shifted = b.build_shl(hi_byte, gb.const_i32(8));
    Value* u16_val = b.build_or(lo_byte, hi_shifted);
    // Redundant mask `(lo | (hi << 8)) & 0xFFFF` that `run_mir_superopt` proves redundant and elides.
    return b.build_and(u16_val, gb.const_i32(0xFFFF));
}

// Emits `regex_dfa_utf16_step` into `mod` using `brass::codegen::GrammarBuilder`.
// Combines UTF-16 byte reconstruction `(lo & 0xFF) | ((hi & 0xFF) << 8)`, redundant
// `& 0xFFFF` mask elision, non-ASCII guard, state bounds guard, and `dfa_table_step`.
void emitDfaUtf16StepFunction(brass::codegen::GrammarBuilder& gb, std::string_view name) {
    using namespace brass;
    Builder& b = gb.builder();
    Module* mod = b.current_module();
    Function* fn = mod->create_function(
        name, Type::i32(), {Type::ptr(), Type::i32(), Type::i32(), Type::i32(), Type::i32()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);
    Value* trans_table = b.add_block_param(entry, Type::ptr());
    Value* num_states = b.add_block_param(entry, Type::i32());
    Value* cur_state = b.add_block_param(entry, Type::i32());
    Value* lo_in = b.add_block_param(entry, Type::i32());
    Value* hi_in = b.add_block_param(entry, Type::i32());

    BasicBlock* valid_bb = b.create_block("valid_step");
    BasicBlock* invalid_bb = b.create_block("invalid_step");

    Value* lo = b.build_and(lo_in, gb.const_i32(0xFF));
    Value* hi = b.build_and(hi_in, gb.const_i32(0xFF));
    Value* u16_val = b.build_or(lo, b.build_shl(hi, gb.const_i32(8)));
    Value* u32_masked = b.build_and(u16_val, gb.const_i32(0xFFFF));
    Value* is_non_ascii = b.build_uge(u32_masked, gb.const_i32(128));
    Value* is_oob = b.build_uge(cur_state, num_states);
    Value* bail = b.build_or(is_non_ascii, is_oob);
    b.build_br_if(bail, invalid_bb, valid_bb);

    fn->append_block(invalid_bb);
    b.position_at_end(invalid_bb);
    b.build_ret(gb.const_i32(-1));

    fn->append_block(valid_bb);
    b.position_at_end(valid_bb);
    Value* ascii_byte = b.build_and(u32_masked, gb.const_i32(0x7F));
    Value* next_state = gb.dfa_table_step(trans_table, cur_state, ascii_byte);
    b.build_ret(next_state);
}

// Emits `regex_dfa_match_at` into `mod` using `brass::codegen::GrammarBuilder`.
void emitDfaMatchAtFunction(brass::codegen::GrammarBuilder& gb, std::string_view name) {
    using namespace brass;
    Builder& b = gb.builder();
    Module* mod = b.current_module();
    Function* fn = mod->create_function(
        name, Type::i64(),
        {Type::ptr(), Type::ptr(), Type::i32(), Type::ptr(), Type::i64(), Type::i64(), Type::ptr()});
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);

    Value* trans_table = b.add_block_param(entry, Type::ptr());
    Value* accept_table = b.add_block_param(entry, Type::ptr());
    Value* start_state = b.add_block_param(entry, Type::i32());
    Value* units_ptr = b.add_block_param(entry, Type::ptr());
    Value* length = b.add_block_param(entry, Type::i64());
    Value* start_pos = b.add_block_param(entry, Type::i64());
    Value* out_end_ptr = b.add_block_param(entry, Type::ptr());

    BasicBlock* valid_start_bb = b.create_block("valid_start");
    BasicBlock* early_no_match_bb = b.create_block("early_no_match");
    BasicBlock* loop_head_bb = b.create_block("loop_head");
    BasicBlock* check_char_bb = b.create_block("check_char");
    BasicBlock* step_dfa_bb = b.create_block("step_dfa");
    BasicBlock* check_accept_bb = b.create_block("check_accept");
    BasicBlock* non_ascii_bb = b.create_block("non_ascii_bailout");
    BasicBlock* finish_bb = b.create_block("finish");
    BasicBlock* write_match_bb = b.create_block("write_match");
    BasicBlock* ret_no_match_bb = b.create_block("ret_no_match");

    // Check if start_state < 0 or start_pos > length
    Value* state_neg = b.build_slt(start_state, gb.const_i32(0));
    Value* pos_oob = b.build_sgt(start_pos, length);
    Value* bad_entry = b.build_or(state_neg, pos_oob);
    b.build_br_if(bad_entry, early_no_match_bb, valid_start_bb);

    fn->append_block(early_no_match_bb);
    b.position_at_end(early_no_match_bb);
    b.build_ret(gb.const_i64(0));

    // Check initial state acceptance at `start_pos`
    fn->append_block(valid_start_bb);
    b.position_at_end(valid_start_bb);
    Value* start_state_64 = b.build_sext_i64(start_state);
    Value* init_acc = gb.load_i32_indexed(accept_table, start_state_64, 4, 0);
    Value* init_is_acc = b.build_sgt(init_acc, gb.const_i32(0));
    Value* neg_one = gb.const_i64(-1);
    Value* init_best_end = b.build_select(init_is_acc, start_pos, neg_one);
    Value* init_stop = b.build_eq(init_acc, gb.const_i32(2));
    b.build_br_if(init_stop, finish_bb, {init_best_end}, loop_head_bb, {start_pos, start_state, init_best_end});

    // Loop header: (cur_pos: i64, cur_state: i32, best_end: i64)
    fn->append_block(loop_head_bb);
    b.position_at_end(loop_head_bb);
    Value* cur_pos = b.add_block_param(loop_head_bb, Type::i64());
    Value* cur_state = b.add_block_param(loop_head_bb, Type::i32());
    Value* best_end = b.add_block_param(loop_head_bb, Type::i64());

    Value* has_more = b.build_slt(cur_pos, length);
    b.build_br_if(has_more, check_char_bb, {}, finish_bb, {best_end});

    // Load 16-bit UTF-16 code unit: uint16_t u = units[cur_pos]
    fn->append_block(check_char_bb);
    b.position_at_end(check_char_bb);
    Value* u32_masked = emitLoadUtf16Unit(gb, units_ptr, cur_pos);
    Value* is_non_ascii = b.build_uge(u32_masked, gb.const_i32(128));
    b.build_br_if(is_non_ascii, non_ascii_bb, step_dfa_bb);

    fn->append_block(non_ascii_bb);
    b.position_at_end(non_ascii_bb);
    b.build_ret(gb.const_i64(-1));

    // Step DFA using GrammarBuilder::dfa_table_step
    fn->append_block(step_dfa_bb);
    b.position_at_end(step_dfa_bb);
    Value* ascii_byte = b.build_and(u32_masked, gb.const_i32(0x7F));
    Value* next_state = gb.dfa_table_step(trans_table, cur_state, ascii_byte);
    Value* is_dead = b.build_slt(next_state, gb.const_i32(0));
    b.build_br_if(is_dead, finish_bb, {best_end}, check_accept_bb, {});

    // Check if `next_state` is accepting
    fn->append_block(check_accept_bb);
    b.position_at_end(check_accept_bb);
    Value* next_pos = b.build_add(cur_pos, gb.const_i64(1));
    Value* next_state_64 = b.build_sext_i64(next_state);
    Value* acc_val = gb.load_i32_indexed(accept_table, next_state_64, 4, 0);
    Value* is_acc = b.build_sgt(acc_val, gb.const_i32(0));
    Value* updated_best = b.build_select(is_acc, next_pos, best_end);
    Value* stop_now = b.build_eq(acc_val, gb.const_i32(2));
    b.build_br_if(stop_now, finish_bb, {updated_best}, loop_head_bb, {next_pos, next_state, updated_best});

    // Finish: check if `final_best >= 0`
    fn->append_block(finish_bb);
    b.position_at_end(finish_bb);
    Value* final_best = b.add_block_param(finish_bb, Type::i64());
    Value* matched = b.build_sge(final_best, gb.const_i64(0));
    b.build_br_if(matched, write_match_bb, ret_no_match_bb);

    fn->append_block(write_match_bb);
    b.position_at_end(write_match_bb);
    gb.store_i64(out_end_ptr, final_best, 0);
    b.build_ret(gb.const_i64(1));

    fn->append_block(ret_no_match_bb);
    b.position_at_end(ret_no_match_bb);
    b.build_ret(gb.const_i64(0));
}

// Emits `regex_dfa_search` into `mod` using `brass::codegen::GrammarBuilder`.
// Scans start positions `at` in `[from_pos, length]` in native machine code!
void emitDfaSearchFunction(brass::codegen::GrammarBuilder& gb, std::string_view name) {
    using namespace brass;
    Builder& b = gb.builder();
    Module* mod = b.current_module();
    Function* fn = mod->create_function(name, Type::i64(), {
        Type::ptr(), // transition_table
        Type::ptr(), // accept_table
        Type::i32(), // start_state
        Type::ptr(), // units (const uint16_t*)
        Type::i64(), // length
        Type::i64(), // from_pos
        Type::ptr(), // out_match_start (int64_t*)
        Type::ptr()  // out_match_end (int64_t*)
    });
    b.set_function(fn);

    BasicBlock* entry = b.append_block("entry");
    b.position_at_end(entry);

    Value* trans_table = b.add_block_param(entry, Type::ptr());
    Value* accept_table = b.add_block_param(entry, Type::ptr());
    Value* start_state = b.add_block_param(entry, Type::i32());
    Value* units_ptr = b.add_block_param(entry, Type::ptr());
    Value* length = b.add_block_param(entry, Type::i64());
    Value* from_pos = b.add_block_param(entry, Type::i64());
    Value* out_start_ptr = b.add_block_param(entry, Type::ptr());
    Value* out_end_ptr = b.add_block_param(entry, Type::ptr());

    BasicBlock* early_no_match_bb = b.create_block("early_no_match");
    BasicBlock* init_bb = b.create_block("init_search");
    BasicBlock* outer_head_bb = b.create_block("outer_head");
    BasicBlock* outer_body_bb = b.create_block("outer_body");
    BasicBlock* inner_head_bb = b.create_block("inner_head");
    BasicBlock* inner_char_bb = b.create_block("inner_char");
    BasicBlock* inner_step_bb = b.create_block("inner_step");
    BasicBlock* inner_acc_bb = b.create_block("inner_acc");
    BasicBlock* inner_done_bb = b.create_block("inner_done");
    BasicBlock* outer_next_bb = b.create_block("outer_next");
    BasicBlock* check_next_at_unit_bb = b.create_block("check_next_at_unit");
    BasicBlock* advance_at_bb = b.create_block("advance_at");
    BasicBlock* found_match_bb = b.create_block("found_match");
    BasicBlock* non_ascii_bb = b.create_block("non_ascii_bailout");

    Value* state_neg = b.build_slt(start_state, gb.const_i32(0));
    Value* pos_oob = b.build_sgt(from_pos, length);
    Value* bad_entry = b.build_or(state_neg, pos_oob);
    b.build_br_if(bad_entry, early_no_match_bb, init_bb);

    fn->append_block(early_no_match_bb);
    b.position_at_end(early_no_match_bb);
    b.build_ret(gb.const_i64(0));

    fn->append_block(init_bb);
    b.position_at_end(init_bb);
    Value* start_state_64 = b.build_sext_i64(start_state);
    Value* init_acc = gb.load_i32_indexed(accept_table, start_state_64, 4, 0);
    Value* init_is_acc = b.build_sgt(init_acc, gb.const_i32(0));
    Value* init_stop = b.build_eq(init_acc, gb.const_i32(2));
    Value* neg_one = gb.const_i64(-1);
    b.build_br(outer_head_bb, {from_pos});

    // Outer loop over `at` in `[from_pos, length]`
    fn->append_block(outer_head_bb);
    b.position_at_end(outer_head_bb);
    Value* at = b.add_block_param(outer_head_bb, Type::i64());
    Value* at_in_bounds = b.build_sle(at, length);
    b.build_br_if(at_in_bounds, outer_body_bb, early_no_match_bb);

    fn->append_block(outer_body_bb);
    b.position_at_end(outer_body_bb);
    Value* seed_best = b.build_select(init_is_acc, at, neg_one);
    b.build_br_if(init_stop, inner_done_bb, {seed_best}, inner_head_bb, {at, start_state, seed_best});

    // Inner loop over `cur_pos` from `at` to `length`
    fn->append_block(inner_head_bb);
    b.position_at_end(inner_head_bb);
    Value* cur_pos = b.add_block_param(inner_head_bb, Type::i64());
    Value* cur_state = b.add_block_param(inner_head_bb, Type::i32());
    Value* best_end = b.add_block_param(inner_head_bb, Type::i64());

    Value* has_more = b.build_slt(cur_pos, length);
    b.build_br_if(has_more, inner_char_bb, {}, inner_done_bb, {best_end});

    fn->append_block(inner_char_bb);
    b.position_at_end(inner_char_bb);
    Value* u32_masked = emitLoadUtf16Unit(gb, units_ptr, cur_pos);
    Value* is_non_ascii = b.build_uge(u32_masked, gb.const_i32(128));
    b.build_br_if(is_non_ascii, non_ascii_bb, inner_step_bb);

    fn->append_block(non_ascii_bb);
    b.position_at_end(non_ascii_bb);
    b.build_ret(gb.const_i64(-1));

    fn->append_block(inner_step_bb);
    b.position_at_end(inner_step_bb);
    Value* ascii_byte = b.build_and(u32_masked, gb.const_i32(0x7F));
    Value* next_state = gb.dfa_table_step(trans_table, cur_state, ascii_byte);
    Value* is_dead = b.build_slt(next_state, gb.const_i32(0));
    b.build_br_if(is_dead, inner_done_bb, {best_end}, inner_acc_bb, {});

    fn->append_block(inner_acc_bb);
    b.position_at_end(inner_acc_bb);
    Value* next_pos = b.build_add(cur_pos, gb.const_i64(1));
    Value* next_state_64 = b.build_sext_i64(next_state);
    Value* acc_val = gb.load_i32_indexed(accept_table, next_state_64, 4, 0);
    Value* is_acc = b.build_sgt(acc_val, gb.const_i32(0));
    Value* updated_best = b.build_select(is_acc, next_pos, best_end);
    Value* stop_now = b.build_eq(acc_val, gb.const_i32(2));
    b.build_br_if(stop_now, inner_done_bb, {updated_best}, inner_head_bb, {next_pos, next_state, updated_best});

    // Inner loop done: check if a match was found starting at `at`
    fn->append_block(inner_done_bb);
    b.position_at_end(inner_done_bb);
    Value* final_best = b.add_block_param(inner_done_bb, Type::i64());
    Value* matched = b.build_sge(final_best, gb.const_i64(0));
    b.build_br_if(matched, found_match_bb, outer_next_bb);

    // If `at < length`, verify that `units[at]` is ASCII before advancing `at`
    // (in case `start_state` died on the first character or before reading `units[at]`).
    fn->append_block(outer_next_bb);
    b.position_at_end(outer_next_bb);
    Value* at_lt_len = b.build_slt(at, length);
    b.build_br_if(at_lt_len, check_next_at_unit_bb, advance_at_bb);

    fn->append_block(check_next_at_unit_bb);
    b.position_at_end(check_next_at_unit_bb);
    Value* at_u32 = emitLoadUtf16Unit(gb, units_ptr, at);
    Value* at_non_ascii = b.build_uge(at_u32, gb.const_i32(128));
    b.build_br_if(at_non_ascii, non_ascii_bb, advance_at_bb);

    fn->append_block(advance_at_bb);
    b.position_at_end(advance_at_bb);
    Value* next_at = b.build_add(at, gb.const_i64(1));
    b.build_br(outer_head_bb, {next_at});

    fn->append_block(found_match_bb);
    b.position_at_end(found_match_bb);
    gb.store_i64(out_start_ptr, at, 0);
    gb.store_i64(out_end_ptr, final_best, 0);
    b.build_ret(gb.const_i64(1));
}

// Process-wide singleton holding the compiled native `KernelJit` functions for the regex DFA engine.
struct SharedRegexJitKernels {
    std::unique_ptr<brass::Module> mod;
    std::unique_ptr<brass::codegen::KernelJit> jit;
    brass::codegen::KernelFunction match_at_kf;
    brass::codegen::KernelFunction search_kf;
    RegexDfaMatchAtFn match_at_fn = nullptr;
    RegexDfaSearchFn search_fn = nullptr;
    brass::SmtGuardElimStats guard_stats{};
    brass::SuperoptStats superopt_stats{};

    SharedRegexJitKernels() {
        mod = RegexJitPlan::buildMirModule();
        // The SMT passes prove away one redundant UTF-16 mask and cost some
        // 200 ms of solver time, in every process that compiles a regex and
        // again at its exit, which joins this thread. They run here only
        // when asked, as in the module backend (brass_backend.cpp);
        // regex_jit_test proves what they would elide either way.
        const char* smtOpt = std::getenv("BRONZE_SMT_OPT");
        const bool smtOptEnabled = smtOpt && std::string_view(smtOpt) == "1";
        if (smtOptEnabled) RegexJitPlan::optimizeMirModule(*mod, &guard_stats, &superopt_stats);

        brass::codegen::KernelOptions kopts;
        kopts.enable_optimizations = true;
        kopts.enable_smt_guard_elim = smtOptEnabled;
        kopts.enable_superopt = smtOptEnabled;
        kopts.enable_unroll = false;
        kopts.enable_vectorize = false;
        jit = std::make_unique<brass::codegen::KernelJit>(kopts);

        match_at_kf = jit->compile(*mod, "regex_dfa_match_at");
        search_kf = jit->compile(*mod, "regex_dfa_search");
        match_at_fn = match_at_kf.as<RegexDfaMatchAtFn>();
        search_fn = search_kf.as<RegexDfaSearchFn>();
    }
};

// Built once, on a thread of its own (RegexJitPlan::waitForKernels): the page
// thread that compiles the first regex never waits on the solver.
struct KernelBuild {
    std::atomic<const SharedRegexJitKernels*> ready{nullptr};
    std::unique_ptr<SharedRegexJitKernels> kernels;
    std::mutex mu;
    std::condition_variable cv;
    std::thread worker;

    KernelBuild() {
        worker = std::thread([this] {
            auto k = std::make_unique<SharedRegexJitKernels>();
            std::lock_guard<std::mutex> lock(mu);
            kernels = std::move(k);
            ready.store(kernels.get(), std::memory_order_release);
            cv.notify_all();
        });
    }
    // Joined at exit, before the statics the build uses go.
    ~KernelBuild() {
        if (worker.joinable()) worker.join();
    }
};

KernelBuild& kernelBuild() {
    static KernelBuild build;
    return build;
}

// The kernels, or null while they are still being built (started here).
const SharedRegexJitKernels* readyKernels() {
    return kernelBuild().ready.load(std::memory_order_acquire);
}

}  // namespace

std::unique_ptr<brass::Module> RegexJitPlan::buildMirModule(
    const int32_t* transition_table,
    int32_t num_states) {
    auto mod = std::make_unique<brass::Module>("bronze_regex_dfa_jit");
    brass::codegen::GrammarBuilder gb(*mod);

    // 1. Emit the table-driven single-step function via GrammarBuilder
    gb.build_dfa_table_step_function("regex_dfa_table_step");

    // 2. If a concrete transition table is provided, also emit the specialized immediate-table step kernel
    if (transition_table != nullptr && num_states > 0) {
        gb.build_dfa_step_function(transition_table, num_states, "regex_dfa_step_specialized");
    }

    // 3. Emit the unrolled byte string scanner and UTF-16 step kernel via GrammarBuilder
    gb.build_dfa_scan_string_function("regex_dfa_scan_bytes", 1);
    emitDfaUtf16StepFunction(gb, "regex_dfa_utf16_step");

    // 4. Emit the UTF-16 `matchAt` and `search` DFA kernels
    emitDfaMatchAtFunction(gb, "regex_dfa_match_at");
    emitDfaSearchFunction(gb, "regex_dfa_search");

    for (brass::Function* fn : mod->functions()) {
        if (fn) fn->rebuild_cfg_predecessors();
    }
    return mod;
}

void RegexJitPlan::optimizeMirModule(
    brass::Module& mod,
    brass::SmtGuardElimStats* guard_stats,
    brass::SuperoptStats* superopt_stats) {
    for (brass::Function* fn : mod.functions()) {
        if (fn) fn->rebuild_cfg_predecessors();
    }

    brass::SmtGuardElimOptions gopts;
    gopts.stats = guard_stats;
    brass::run_smt_guard_elimination(mod, gopts);

    brass::SuperoptOptions sopts;
    sopts.stats = superopt_stats;
    brass::run_mir_superopt(mod, sopts);

    for (brass::Function* fn : mod.functions()) {
        if (fn) {
            fn->rebuild_cfg_predecessors();
        }
    }
}

std::unique_ptr<RegexJitPlan> RegexJitPlan::tryCompile(const Pattern& pattern) {
    if (!pattern.root) {
        return nullptr;
    }

    NfaCompiler nfa_compiler(pattern.flags);
    bool anchors_at_start = false;
    if (!nfa_compiler.compileRoot(*pattern.root, anchors_at_start)) {
        return nullptr;
    }

    auto plan = std::make_unique<RegexJitPlan>();
    plan->requires_capture_extraction_ = (pattern.groupCount > 0);
    plan->anchors_at_start_ = anchors_at_start;
    plan->stats_.nfa_states = static_cast<uint32_t>(nfa_compiler.insts().size());

    plan->transition_table_.assign(256, -1);
    DfaBuilder dfa_builder(nfa_compiler.insts());
    if (!dfa_builder.build(
            nfa_compiler.startState(),
            plan->transition_table_,
            plan->accept_table_,
            plan->start_state_,
            plan->num_dfa_states_)) {
        return nullptr;
    }

    // Starts the shared kernels' build if nothing has yet.
    (void)readyKernels();
    plan->stats_.dfa_states = static_cast<uint32_t>(plan->num_dfa_states_);
    plan->supported_ = true;
    return plan;
}

void RegexJitPlan::waitForKernels() {
    KernelBuild& b = kernelBuild();
    std::unique_lock<std::mutex> lock(b.mu);
    b.cv.wait(lock, [&b] { return b.ready.load(std::memory_order_acquire) != nullptr; });
}

RegexJitStats RegexJitPlan::stats() const noexcept {
    RegexJitStats s = stats_;
    if (const SharedRegexJitKernels* k = readyKernels()) {
        s.smt_guards_eliminated = static_cast<uint32_t>(k->guard_stats.guards_proven_eliminated);
        s.superopt_masks_elided = k->superopt_stats.masks_elided;
        s.superopt_extensions_elided = k->superopt_stats.extensions_elided;
        s.superopt_dags_synthesized = k->superopt_stats.dags_synthesized;
        s.superopt_solver_queries = k->superopt_stats.solver_queries;
    }
    return s;
}

bool RegexJitPlan::tryMatchAt(UnitsView input, size_t at, MatchResult& out, ExecStatus& status) const {
    const SharedRegexJitKernels* k = supported_ ? readyKernels() : nullptr;
    if (!k || !k->match_at_fn) return false;
    if (at > input.size() || (anchors_at_start_ && at != 0) || start_state_ < 0 || num_dfa_states_ <= 0) {
        status = ExecStatus::NoMatch;
        return true;
    }

    int64_t match_end = -1;
    const uint16_t* raw_units = input.empty() ? nullptr : reinterpret_cast<const uint16_t*>(input.data());
    const int64_t rc = k->match_at_fn(
        transition_table_.data(), accept_table_.data(), start_state_,
        raw_units, static_cast<int64_t>(input.size()), static_cast<int64_t>(at), &match_end);

    if (rc < 0) return false; // Non-ASCII code unit encountered; fall back to full UTF-16 interpreter.
    if (rc == 0) {
        status = ExecStatus::NoMatch;
        return true;
    }
    out.captures.assign({static_cast<int64_t>(at), match_end});
    status = ExecStatus::Match;
    return true;
}

bool RegexJitPlan::trySearch(UnitsView input, size_t from, MatchResult& out, ExecStatus& status) const {
    const SharedRegexJitKernels* k = supported_ ? readyKernels() : nullptr;
    if (!k || !k->search_fn) return false;
    if (from > input.size() || start_state_ < 0 || num_dfa_states_ <= 0) {
        status = ExecStatus::NoMatch;
        return true;
    }
    if (anchors_at_start_) {
        if (from > 0) {
            status = ExecStatus::NoMatch;
            return true;
        }
        return tryMatchAt(input, 0, out, status);
    }

    int64_t match_start = -1;
    int64_t match_end = -1;
    const uint16_t* raw_units = input.empty() ? nullptr : reinterpret_cast<const uint16_t*>(input.data());
    const int64_t rc = k->search_fn(
        transition_table_.data(), accept_table_.data(), start_state_,
        raw_units, static_cast<int64_t>(input.size()), static_cast<int64_t>(from),
        &match_start, &match_end);

    if (rc < 0) return false; // Non-ASCII code unit encountered; fall back to full UTF-16 interpreter.
    if (rc == 0) {
        status = ExecStatus::NoMatch;
        return true;
    }
    out.captures.assign({match_start, match_end});
    status = ExecStatus::Match;
    return true;
}

bool hasJitFastPath(const Pattern& pattern) noexcept {
    return pattern.jitPlan != nullptr && pattern.jitPlan->isSupported();
}

}  // namespace bronze::regex
