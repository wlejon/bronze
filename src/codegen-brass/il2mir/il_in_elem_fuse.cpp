// `k in O` FUSED WITH `O[k]`: the self-hosted Array builtins (src/selfhost)
// ask HasProperty and then Get of the same index on every iteration,
//
//     A:  %h = in %k1, %O            ; %k1 = box.f64 %x
//         br %h, B, ...
//     B:  %v = elem.get %O, %k2      ; %k2 = box.f64 %x
//
// and both lowerings load the same header, bounds and element. The `in`'s
// inline arm answers true only for an Array's own element in its block — an
// in-bounds slot that is no hole, which is a data property whose value is the
// slot — so on that arm the read's answer is the slot it loaded. The `in`
// hands the slot over (the hole when its inline arm did not answer: the
// helper's `true` came from the chain, a proxy or a key that is no index), and
// the read takes it when it is no hole and is its own full lowering otherwise.
//
// What makes the hand-over sound is what stands between the two: `B` is
// entered only by the true edge, and every instruction after the `in` in `A`
// and before the read in `B` is a box or a constant, which neither writes an
// object nor collects, so the slot is still the element and its bits still
// name the same object when the read uses them.

#include "il_lowering.h"
#include <unordered_map>
#include <unordered_set>

namespace il2mir {

namespace {

// A box of a primitive or a constant: writes no object, runs nothing, and
// allocates nothing (a String box calls out, so it is not one).
bool quiet(const BronzeInstruction& inst) {
    switch (inst.op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
            return true;
        case BronzeOp::Box:
            return inst.box_type == BronzeType::F64 || inst.box_type == BronzeType::I32 ||
                   inst.box_type == BronzeType::Bool;
        default:
            return false;
    }
}

}  // namespace

void IlLowering::pair_in_element_reads(const BronzeFunction& fn_ast) {
    in_element_reads_.clear();
    in_elements_.clear();

    std::unordered_map<uint32_t, const BronzeBlock*> blocks;
    std::unordered_map<uint32_t, uint32_t> incoming;
    std::unordered_set<uint32_t> handlers;
    std::unordered_map<uint32_t, const BronzeInstruction*> defs;
    for (const auto& blk : fn_ast.blocks) {
        blocks[blk.id] = &blk;
        if (blk.handler_id != UINT32_MAX) handlers.insert(blk.handler_id);
        for (const auto& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX) defs[inst.result_id] = &inst;
            if (inst.op == BronzeOp::Jump) ++incoming[inst.target.block_id];
            if (inst.op == BronzeOp::Branch) {
                ++incoming[inst.target.block_id];
                ++incoming[inst.else_target.block_id];
            }
        }
    }
    // The key's number: the boxed operand of a `box.f64`, else the key itself.
    auto key_of = [&](uint32_t id) {
        const auto it = defs.find(id);
        if (it != defs.end() && it->second->op == BronzeOp::Box && it->second->box_type == BronzeType::F64 &&
            it->second->operands.size() == 1) {
            return it->second->operands[0];
        }
        return id;
    };

    for (const auto& blk : fn_ast.blocks) {
        const auto& insts = blk.instructions;
        if (insts.empty() || insts.back().op != BronzeOp::Branch || insts.back().operands.empty()) continue;
        const BronzeInstruction& br = insts.back();
        // The `in` the branch tests, with only quiet instructions after it.
        size_t at = insts.size() - 1;
        while (at > 0 && quiet(insts[at - 1])) --at;
        if (at == 0) continue;
        const BronzeInstruction& in = insts[at - 1];
        if (in.op != BronzeOp::In || in.operands.size() != 2 || br.operands[0] != in.result_id) continue;
        const uint32_t succ = br.target.block_id;
        if (succ == br.else_target.block_id || incoming[succ] != 1 || handlers.count(succ) != 0) continue;
        const auto sit = blocks.find(succ);
        if (sit == blocks.end() || !sit->second->params.empty()) continue;
        // The read, first after quiet instructions in the true successor.
        for (const auto& inst : sit->second->instructions) {
            if (quiet(inst)) continue;
            if (inst.op == BronzeOp::ElemGet && inst.operands.size() == 2 && inst.result_id != UINT32_MAX &&
                inst.operands[0] == in.operands[1] && key_of(inst.operands[1]) == key_of(in.operands[0])) {
                in_element_reads_[inst.result_id] = in.result_id;
            }
            break;
        }
    }
}

bool IlLowering::in_loads_element(uint32_t in_id) const {
    for (const auto& pair : in_element_reads_) {
        if (pair.second == in_id) return true;
    }
    return false;
}

Value* IlLowering::in_element_for(uint32_t get_id) const {
    const auto pair = in_element_reads_.find(get_id);
    if (pair == in_element_reads_.end()) return nullptr;
    const auto elem = in_elements_.find(pair->second);
    return elem == in_elements_.end() ? nullptr : elem->second;
}

}  // namespace il2mir
