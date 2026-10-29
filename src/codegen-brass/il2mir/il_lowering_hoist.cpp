// What a function's lowering decides once, at its entry: the values every
// inline path shares (hoisted), and which of its blocks run once
// (IlLowering::run_once).

#include "il_lowering.h"
#include "il_cfg_cycles.h"
#include "il/key.h"
#include "il_abi.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il2mir {

void IlLowering::begin_hoisting(BasicBlock* block) {
    hoist_block_ = block;
    hoist_after_ = block ? block->tail() : nullptr;
    // An empty block: the values go in front of whatever it gets later.
    hoist_at_head_ = block && !hoist_after_;
    for (Value*& v : hoisted_) v = nullptr;
    ta_cursors_.clear();
}

Value* IlLowering::hoisted(Builder& b, Hoist kind) {
    Value*& slot = hoisted_[static_cast<size_t>(kind)];
    if (slot) return slot;
    // With no entry to hoist into (a wrapper), each use computes its own.
    BasicBlock* resume = b.current_block();
    if (hoist_block_) {
        if (hoist_after_) {
            b.position_after(hoist_after_);
        } else if (hoist_at_head_ && hoist_block_->head()) {
            b.position_before(hoist_block_->head());
        } else {
            b.position_at_end(hoist_block_);
        }
    }
    Value* v = nullptr;
    switch (kind) {
        case Hoist::IcTable:
            v = module_data_addr(b, "__bronze_ic_table");
            break;
        case Hoist::Count:
            break;
    }
    if (!hoist_block_) {
        return v;
    }
    hoist_after_ = v->defining_instruction();
    b.position_at_end(resume);
    slot = v;
    return v;
}

// Declared in il_lowering.h.
bool is_pure_scalar_inst(const BronzeInstruction& inst) {
    switch (inst.op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
        case BronzeOp::IsNumber:
        case BronzeOp::NumTruthy:
        case BronzeOp::MathImul:
            return true;
        case BronzeOp::Box:
            return inst.box_type == BronzeType::F64 || inst.box_type == BronzeType::I32 ||
                   inst.box_type == BronzeType::Bool;
        case BronzeOp::Unbox:
            return (inst.result_type == BronzeType::F64 && inst.raw_unbox) ||
                   inst.result_type == BronzeType::I32;
        case BronzeOp::Add:
        case BronzeOp::Sub:
        case BronzeOp::Mul:
        case BronzeOp::Div:
        case BronzeOp::Neg:
        case BronzeOp::BitAnd:
        case BronzeOp::BitOr:
        case BronzeOp::BitXor:
        case BronzeOp::Shl:
        case BronzeOp::Shr:
        case BronzeOp::UShr:
        case BronzeOp::BitNot:
        case BronzeOp::CmpLt:
        case BronzeOp::CmpLe:
        case BronzeOp::CmpGt:
        case BronzeOp::CmpGe:
        case BronzeOp::CmpEq:
        case BronzeOp::CmpNe:
            return inst.result_type == BronzeType::F64 || inst.result_type == BronzeType::I32 ||
                   inst.result_type == BronzeType::Bool;
        default:
            return false;
    }
}

namespace {

bool is_pure_or_proven_read(const BronzeInstruction& inst,
                            const std::unordered_map<uint32_t, uint32_t>& proofs,
                            const PropertyLoweringHelper& prop_lowering) {
    // A number-array claim may rewrite an Int32 box as the same Number, which
    // changes no element's value, no length and no block: the proofs stand.
    if (is_pure_scalar_inst(inst) || inst.op == BronzeOp::IsDenseArray || inst.op == BronzeOp::IsNumberArray ||
        inst.op == BronzeOp::MathUnary) {
        return true;
    }
    if (inst.op == BronzeOp::PropGet && inst.static_slot == UINT32_MAX &&
        inst.index != PropertyLoweringHelper::kNoKey && !inst.operands.empty()) {
        if (const auto elem_idx = bronze::il::parseIndexKey(prop_lowering.key_tag(inst.index))) {
            const auto it = proofs.find(inst.operands[0]);
            if (it != proofs.end() && *elem_idx <= it->second) return true;
        }
    }
    return false;
}

bool is_primitive_result(const BronzeInstruction& inst, const std::unordered_set<uint32_t>& prim_ids) {
    if (inst.result_type == BronzeType::F64 || inst.result_type == BronzeType::I32 ||
        inst.result_type == BronzeType::Bool) {
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
        case BronzeOp::MathImul:
        case BronzeOp::MathUnary:
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
    run_once_ = false;
    run_once_blocks_.clear();
    loop_blocks_.clear();
    {
        const std::vector<uint8_t> cyclic = cyclic_blocks(fn_ast);
        for (size_t i = 0; i < cyclic.size(); ++i) {
            if (cyclic[i]) loop_blocks_.insert(fn_ast.blocks[i].id);
        }
    }
    fresh_objects_.clear();
    hole_insensitive_ids_.clear();
    unrooted_hole_insensitive_ids_.clear();
    primitive_value_ids_.clear();
    number_value_ids_.clear();
    current_fn_dynamic_ids_.clear();
    number_bits_ids_.clear();
    block_dense_proofs_.clear();
    active_dense_arrays_.clear();
    dense_array_cursors_.clear();
    single_block_propset_use_.clear();
    value_use_counts_.clear();
    prop_get_keys_.clear();

    for (const auto& p : fn_ast.params) {
        if (p.second == BronzeType::F64 || p.second == BronzeType::I32 || p.second == BronzeType::Bool) {
            primitive_value_ids_.insert(p.first);
        }
    }

    struct PropGetDef {
        uint32_t block_id = UINT32_MAX;
        size_t inst_idx = 0;
        uint32_t use_count = 0;
        bool is_index_key = false;
        bool all_hole_insensitive = true;
        bool passed_to_block_param = false;
        size_t propset_use_idx = SIZE_MAX;
    };
    std::unordered_map<uint32_t, PropGetDef> propget_defs;
    std::unordered_map<uint32_t, DenseArrayProof> dense_defs;
    std::unordered_map<uint32_t, uint32_t> pred_count;
    std::unordered_map<uint32_t, const BronzeBlock*> block_by_id;

    if (!fn_ast.blocks.empty()) {
        pred_count[fn_ast.blocks[0].id] = 1;
    }

    for (const auto& blk : fn_ast.blocks) {
        block_by_id[blk.id] = &blk;
        for (const auto& p : blk.params) {
            if (p.second == BronzeType::F64 || p.second == BronzeType::I32 || p.second == BronzeType::Bool) {
                primitive_value_ids_.insert(p.first);
            }
        }
        for_each_successor(blk, [&](uint32_t succ_id) { ++pred_count[succ_id]; });
        for (size_t idx = 0; idx < blk.instructions.size(); ++idx) {
            const auto& inst = blk.instructions[idx];
            for (uint32_t op_id : inst.operands) ++value_use_counts_[op_id];
            for (uint32_t arg_id : inst.target.args) ++value_use_counts_[arg_id];
            for (uint32_t arg_id : inst.else_target.args) ++value_use_counts_[arg_id];
            if (inst.result_id != UINT32_MAX) {
                if (is_primitive_result(inst, primitive_value_ids_)) {
                    primitive_value_ids_.insert(inst.result_id);
                }
                if (inst.op == BronzeOp::Box && inst.box_type == BronzeType::F64) {
                    number_value_ids_.insert(inst.result_id);
                }
                if (inst.op == BronzeOp::PropGet && inst.index != PropertyLoweringHelper::kNoKey) {
                    prop_get_keys_.emplace(inst.result_id, inst.index);
                }
                if (inst.op == BronzeOp::PropGet && inst.static_slot == UINT32_MAX &&
                    inst.index != PropertyLoweringHelper::kNoKey && !inst.operands.empty() &&
                    bronze::il::parseIndexKey(prop_lowering_.key_tag(inst.index)).has_value()) {
                    propget_defs.emplace(inst.result_id,
                                         PropGetDef{blk.id, idx, 0, true, true, false, SIZE_MAX});
                }
                if (inst.op == BronzeOp::IsDenseArray && !inst.operands.empty() && inst.imm_i64 >= 0) {
                    dense_defs[inst.result_id] = DenseArrayProof{
                        inst.operands[0], static_cast<uint32_t>(inst.imm_i64)};
                }
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

    if (!propget_defs.empty()) {
        // Identify block parameters whose uses are all hole-safe (`IsNumber` or `Unbox` to F64/I32/Bool).
        // `rtToNumber(Value::fromHole())` produces `quiet_NaN()`, identical to `rtToNumber(Value::fromUndefined())`,
        // and `bronzeHeapConfig()` ignores `Tag::Hole` (`0xFFF9`) just like `Tag::Undefined` (`0xFFF6`).
        std::unordered_map<uint32_t, std::pair<uint32_t, bool>> block_param_hole_safe;
        for (const auto& blk : fn_ast.blocks) {
            for (const auto& p : blk.params) {
                block_param_hole_safe[p.first] = {0, true};
            }
        }
        if (!block_param_hole_safe.empty()) {
            for (const auto& blk : fn_ast.blocks) {
                for (const auto& inst : blk.instructions) {
                    for (size_t op_i = 0; op_i < inst.operands.size(); ++op_i) {
                        auto pit = block_param_hole_safe.find(inst.operands[op_i]);
                        if (pit == block_param_hole_safe.end()) continue;
                        ++pit->second.first;
                        const bool safe =
                            op_i == 0 &&
                            (inst.op == BronzeOp::IsNumber ||
                             (inst.op == BronzeOp::Unbox &&
                              (inst.result_type == BronzeType::F64 || inst.result_type == BronzeType::I32 ||
                               inst.result_type == BronzeType::Bool)));
                        if (!safe) pit->second.second = false;
                    }
                    for (uint32_t arg_id : inst.target.args) {
                        if (auto pit = block_param_hole_safe.find(arg_id); pit != block_param_hole_safe.end()) {
                            ++pit->second.first;
                            pit->second.second = false;
                        }
                    }
                    for (uint32_t arg_id : inst.else_target.args) {
                        if (auto pit = block_param_hole_safe.find(arg_id); pit != block_param_hole_safe.end()) {
                            ++pit->second.first;
                            pit->second.second = false;
                        }
                    }
                }
            }
        }
        std::unordered_set<uint32_t> hole_safe_param_ids;
        for (const auto& [pid, state] : block_param_hole_safe) {
            if (state.first > 0 && state.second) {
                hole_safe_param_ids.insert(pid);
            }
        }

        auto check_target_args = [&](const BronzeBlockTarget& tgt) {
            const auto bit = block_by_id.find(tgt.block_id);
            const BronzeBlock* dst = (bit != block_by_id.end()) ? bit->second : nullptr;
            for (size_t i = 0; i < tgt.args.size(); ++i) {
                auto it = propget_defs.find(tgt.args[i]);
                if (it == propget_defs.end()) continue;
                PropGetDef& def = it->second;
                ++def.use_count;
                def.passed_to_block_param = true;
                def.propset_use_idx = SIZE_MAX;
                const bool dst_param_safe =
                    def.is_index_key && dst && i < dst->params.size() &&
                    hole_safe_param_ids.count(dst->params[i].first) != 0;
                if (!dst_param_safe) {
                    def.all_hole_insensitive = false;
                }
            }
        };

        for (const auto& blk : fn_ast.blocks) {
            for (size_t idx = 0; idx < blk.instructions.size(); ++idx) {
                const auto& inst = blk.instructions[idx];
                for (size_t op_i = 0; op_i < inst.operands.size(); ++op_i) {
                    auto it = propget_defs.find(inst.operands[op_i]);
                    if (it == propget_defs.end()) continue;
                    PropGetDef& def = it->second;
                    ++def.use_count;
                    const bool same_block_after = (blk.id == def.block_id && idx > def.inst_idx);
                    const bool is_hole_safe_use =
                        def.is_index_key && op_i == 0 &&
                        (inst.op == BronzeOp::IsNumber ||
                         (inst.op == BronzeOp::Unbox &&
                          (inst.result_type == BronzeType::F64 || inst.result_type == BronzeType::I32 ||
                           inst.result_type == BronzeType::Bool)));
                    if (!is_hole_safe_use) {
                        def.all_hole_insensitive = false;
                    }
                    if (same_block_after && inst.op == BronzeOp::PropSet && op_i == 1 &&
                        inst.operands.size() >= 2 && inst.operands[0] != it->first && def.use_count == 1) {
                        def.propset_use_idx = idx;
                    } else {
                        def.propset_use_idx = SIZE_MAX;
                    }
                }
                if (inst.op == BronzeOp::Jump) {
                    check_target_args(inst.target);
                } else if (inst.op == BronzeOp::Branch) {
                    check_target_args(inst.target);
                    check_target_args(inst.else_target);
                }
            }
        }
        for (const auto& [id, def] : propget_defs) {
            if (def.is_index_key && def.use_count > 0 && def.all_hole_insensitive) {
                hole_insensitive_ids_.insert(id);
                if (!def.passed_to_block_param) {
                    unrooted_hole_insensitive_ids_.insert(id);
                }
            }
            if (def.use_count == 1 && def.propset_use_idx != SIZE_MAX) {
                single_block_propset_use_[id] = static_cast<uint32_t>(def.propset_use_idx);
            }
        }
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
                unrooted_hole_insensitive_ids_.count(inst.result_id) == 0 &&
                !is_known_unrooted_primitive_inst(inst)) {
                current_fn_dynamic_ids_.insert(inst.result_id);
            }
        }
    }

    if (!dense_defs.empty()) {
        for (const auto& blk : fn_ast.blocks) {
            if (blk.instructions.empty()) continue;
            const auto& term = blk.instructions.back();
            if (term.op == BronzeOp::Branch && !term.operands.empty()) {
                const auto it = dense_defs.find(term.operands[0]);
                if (it != dense_defs.end() && pred_count[term.target.block_id] == 1) {
                    block_dense_proofs_[term.target.block_id].push_back(it->second);
                }
            }
        }
        for (const auto& blk : fn_ast.blocks) {
            const auto it = block_dense_proofs_.find(blk.id);
            if (it == block_dense_proofs_.end() || blk.instructions.empty()) continue;
            std::unordered_map<uint32_t, uint32_t> proofs;
            for (const DenseArrayProof& p : it->second) {
                proofs[p.arr_id] = std::max(proofs[p.arr_id], p.max_index);
            }
            bool preserves = true;
            for (size_t i = 0; i + 1 < blk.instructions.size(); ++i) {
                if (!is_pure_or_proven_read(blk.instructions[i], proofs, prop_lowering_)) {
                    preserves = false;
                    break;
                }
            }
            if (!preserves) continue;
            const auto& term = blk.instructions.back();
            auto propagate_to = [&](uint32_t succ_id) {
                if (pred_count[succ_id] != 1) return;
                auto& dst = block_dense_proofs_[succ_id];
                for (const auto& [arr_id, max_idx] : proofs) {
                    dst.push_back(DenseArrayProof{arr_id, max_idx});
                }
            };
            if (term.op == BronzeOp::Jump) {
                propagate_to(term.target.block_id);
            } else if (term.op == BronzeOp::Branch) {
                propagate_to(term.target.block_id);
                propagate_to(term.else_target.block_id);
            }
        }
    }

    if (!small_forms_) return;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::CreateObject && inst.result_id != UINT32_MAX) fresh_objects_.insert(inst.result_id);
        }
    }
    if (!fn_ast.is_toplevel) return;
    const std::vector<uint8_t> cyclic = cyclic_blocks(fn_ast);
    for (size_t i = 0; i < cyclic.size(); ++i) {
        if (!cyclic[i]) run_once_blocks_.insert(fn_ast.blocks[i].id);
    }
}

void IlLowering::begin_block_proofs(uint32_t block_id) {
    active_dense_arrays_.clear();
    dense_array_cursors_.clear();
    const auto it = block_dense_proofs_.find(block_id);
    if (it == block_dense_proofs_.end()) return;
    for (const DenseArrayProof& p : it->second) {
        auto [pos, inserted] = active_dense_arrays_.emplace(p.arr_id, p.max_index);
        if (!inserted && p.max_index > pos->second) pos->second = p.max_index;
    }
}

bool IlLowering::is_proven_dense_array(uint32_t arr_id, uint32_t index) const {
    const auto it = active_dense_arrays_.find(arr_id);
    return it != active_dense_arrays_.end() && index <= it->second;
}

void IlLowering::get_dense_array_cursor(uint32_t arr_id, Value* obj_val, Builder& b, Value*& out_block,
                                        Value*& out_head) {
    BasicBlock* cur_bb = b.current_block();
    const auto it = dense_array_cursors_.find(arr_id);
    if (it != dense_array_cursors_.end() && it->second.bb == cur_bb) {
        out_block = it->second.block;
        out_head = it->second.head;
        return;
    }
    prop_lowering_.load_dense_array_cursor(b, obj_val, out_block, out_head);
    dense_array_cursors_[arr_id] = DenseArrayCursor{cur_bb, out_block, out_head};
}

void IlLowering::invalidate_dense_array_proofs() {
    active_dense_arrays_.clear();
    dense_array_cursors_.clear();
}

void IlLowering::after_instruction_proofs(const BronzeInstruction& inst_ast) {
    if (active_dense_arrays_.empty() && dense_array_cursors_.empty()) return;
    if (is_pure_or_proven_read(inst_ast, active_dense_arrays_, prop_lowering_)) return;
    invalidate_dense_array_proofs();
}

} // namespace il2mir
