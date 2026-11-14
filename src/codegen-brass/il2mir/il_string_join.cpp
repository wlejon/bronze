// A `+`-chain accumulator (concat.begin, concat.append ..., concat.end) whose
// operands are all Strings before it starts: a template's quasis and the
// to.string of each substitution. Joining Strings runs nothing, so the order
// 13.15.3 fixes between one operand's conversion and the next one's
// evaluation has nothing left to order, and the whole chain is one call that
// allocates the result once at its length (bronze_concat_strings), in place
// of a builder grown by a call per operand and then sealed.
#include "il_lowering.h"
#include "il_lowering_calls.h"

namespace il2mir {

bool IlLowering::try_lower_string_join(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b,
                                       std::unordered_map<uint32_t, Value*>& val_map) {
    const auto& insts = blk_ast.instructions;
    const BronzeInstruction& begin = insts[inst_idx];
    if (begin.op != BronzeOp::ConcatBegin || begin.operands.size() != 2) return false;
    auto is_string = [&](uint32_t id) { return to_string_ids_.count(id) != 0 || is_string_literal(id); };
    auto single_use = [&](uint32_t id) {
        const auto it = value_use_counts_.find(id);
        return it != value_use_counts_.end() && it->second == 1;
    };
    std::vector<uint32_t> pieces{begin.operands[0], begin.operands[1]};
    uint32_t acc = begin.result_id;
    size_t at = inst_idx + 1;
    for (; at < insts.size() && insts[at].op == BronzeOp::ConcatAppend; ++at) {
        if (insts[at].operands.size() != 2 || insts[at].operands[0] != acc || !single_use(acc)) return false;
        pieces.push_back(insts[at].operands[1]);
        acc = insts[at].result_id;
    }
    if (at >= insts.size() || insts[at].op != BronzeOp::ConcatEnd || insts[at].operands.size() != 1 ||
        insts[at].operands[0] != acc || !single_use(acc)) {
        return false;
    }
    if (pieces.size() < 3 || static_cast<int64_t>(pieces.size()) > kStringJoinMaxPieces ||
        pieces.size() > argv_block_words_) {
        return false;
    }
    for (const uint32_t id : pieces) {
        if (!is_string(id) || is_elided_typeof_lit(id)) return false;
    }

    apply_inst_loc(b, insts[at]);
    std::vector<Value*> values;
    for (const uint32_t id : pieces) values.push_back(get_val_by_id(id, b, val_map));
    Value* argv = stage_argv(b, values);
    Value* count = b.build_iconst_i32(static_cast<int32_t>(pieces.size()));
    Value* joined = b.build_call("bronze_concat_strings", Type::i64(), {argv, count});
    set_inst_result(insts[at].result_id, joined, b, val_map);
    inst_idx = at;
    return true;
}

}  // namespace il2mir
