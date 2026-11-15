// What a function's lowering decides once, before its body: which of its
// values are primitive or held tagged.
//
// The entry computes only the two values every resume point's state carries
// beside the IL's (the argv block and the module delta, il_lowering_resume.cpp):
// a function with a `resume.point` is also entered there by its resume twin,
// which has only the point's state in hand, so anything else the body reads
// is an IL value or computed where it is read (the IC table's address, once
// computed at the entry and shared, is now computed per use).

#include "il_lowering.h"
#include "il_abi.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il2mir {

namespace {

bool is_primitive_result(const BronzeInstruction& inst, const std::unordered_set<uint32_t>& prim_ids) {
    if (inst.result_type == BronzeType::F64 || inst.result_type == BronzeType::I32 ||
        inst.result_type == BronzeType::Bool || inst.result_type == BronzeType::Ptr) {
        return true;
    }
    switch (inst.op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
        case BronzeOp::IsNumber:
        case BronzeOp::IsDenseArray:
        case BronzeOp::IsNullish:
        case BronzeOp::NumTruthy:
        case BronzeOp::ToInt32:
        case BronzeOp::CmpLt:
        case BronzeOp::CmpLe:
        case BronzeOp::CmpGt:
        case BronzeOp::CmpGe:
        case BronzeOp::CmpEq:
        case BronzeOp::CmpNe:
        case BronzeOp::RelLt:
        case BronzeOp::RelLe:
        case BronzeOp::RelGt:
        case BronzeOp::RelGe:
        case BronzeOp::StrictEq:
        case BronzeOp::LooseEq:
        case BronzeOp::UShr:
        case BronzeOp::TypeOf:
            return true;
        // Over operands that may be BigInts these give a BigInt (6.1.6.2), a
        // heap cell: a store of one needs the barrier and a hold of one the
        // root, so the result is primitive only when its operands are.
        case BronzeOp::BitAnd:
        case BronzeOp::BitOr:
        case BronzeOp::BitXor:
        case BronzeOp::Shl:
        case BronzeOp::Shr:
        case BronzeOp::BitNot:
        case BronzeOp::Sub:
        case BronzeOp::Mul:
        case BronzeOp::Div:
        case BronzeOp::Mod:
        case BronzeOp::Pow:
        case BronzeOp::Neg:
            if (inst.operands.empty()) return false;
            for (uint32_t op : inst.operands) {
                if (prim_ids.count(op) == 0) return false;
            }
            return true;
        case BronzeOp::Box:
            if (inst.box_type == BronzeType::F64 || inst.box_type == BronzeType::I32 ||
                inst.box_type == BronzeType::Bool) {
                return true;
            }
            return inst.box_type != BronzeType::Str && !inst.operands.empty() &&
                   prim_ids.count(inst.operands[0]) != 0;
        case BronzeOp::Unbox:
            return inst.result_type != BronzeType::Str;
        case BronzeOp::Add:
            return inst.operands.size() >= 2 &&
                   prim_ids.count(inst.operands[0]) != 0 &&
                   prim_ids.count(inst.operands[1]) != 0;
        default:
            return false;
    }
}

bool is_known_unrooted_primitive_inst(const BronzeInstruction& inst) {
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

} // namespace

void IlLowering::analyze_function(const BronzeFunction& fn_ast) {
    primitive_value_ids_.clear();
    current_fn_dynamic_ids_.clear();

    for (const auto& p : fn_ast.params) {
        if (p.second == BronzeType::F64 || p.second == BronzeType::I32 || p.second == BronzeType::Bool) {
            primitive_value_ids_.insert(p.first);
        }
    }

    std::unordered_map<uint32_t, const BronzeBlock*> block_by_id;
    for (const auto& blk : fn_ast.blocks) {
        block_by_id[blk.id] = &blk;
        for (const auto& p : blk.params) {
            if (p.second == BronzeType::F64 || p.second == BronzeType::I32 || p.second == BronzeType::Bool) {
                primitive_value_ids_.insert(p.first);
            }
        }
        for (const auto& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX && is_primitive_result(inst, primitive_value_ids_)) {
                primitive_value_ids_.insert(inst.result_id);
            }
        }
    }

    // Fixed-point propagation of known primitive values across instructions and block params.
    for (int iter = 0; iter < 4; ++iter) {
        bool changed = false;
        std::unordered_map<uint32_t, std::pair<uint32_t, bool>> param_prim;
        for (const auto& blk : fn_ast.blocks) {
            for (const auto& p : blk.params) {
                if (primitive_value_ids_.count(p.first) == 0) {
                    param_prim[p.first] = {0, true};
                }
            }
        }
        for (const auto& blk : fn_ast.blocks) {
            auto check_target = [&](const BronzeBlockTarget& tgt) {
                if (tgt.args.empty()) return;
                const auto bit = block_by_id.find(tgt.block_id);
                if (bit == block_by_id.end()) return;
                const BronzeBlock& dst = *bit->second;
                const size_t n = std::min(tgt.args.size(), dst.params.size());
                for (size_t i = 0; i < n; ++i) {
                    auto pit = param_prim.find(dst.params[i].first);
                    if (pit == param_prim.end()) continue;
                    ++pit->second.first;
                    if (primitive_value_ids_.count(tgt.args[i]) == 0) {
                        pit->second.second = false;
                    }
                }
            };
            for (const auto& inst : blk.instructions) {
                if (inst.result_id != UINT32_MAX && primitive_value_ids_.count(inst.result_id) == 0 &&
                    is_primitive_result(inst, primitive_value_ids_)) {
                    primitive_value_ids_.insert(inst.result_id);
                    changed = true;
                }
                if (inst.op == BronzeOp::Jump) {
                    check_target(inst.target);
                } else if (inst.op == BronzeOp::Branch) {
                    check_target(inst.target);
                    check_target(inst.else_target);
                }
            }
        }
        for (const auto& [pid, state] : param_prim) {
            if (state.first > 0 && state.second) {
                primitive_value_ids_.insert(pid);
                changed = true;
            }
        }
        if (!changed) break;
    }

    auto is_dynamic = [](BronzeType t) { return t == BronzeType::Dynamic || t == BronzeType::Unknown; };
    for (const auto& p : fn_ast.params) {
        if (is_dynamic(p.second)) current_fn_dynamic_ids_.insert(p.first);
    }
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& p : blk.params) {
            if (is_dynamic(p.second)) current_fn_dynamic_ids_.insert(p.first);
        }
        for (const auto& inst : blk.instructions) {
            if (inst.result_id != UINT32_MAX && is_dynamic(inst.result_type) &&
                !is_known_unrooted_primitive_inst(inst)) {
                current_fn_dynamic_ids_.insert(inst.result_id);
            }
        }
    }
}

} // namespace il2mir
