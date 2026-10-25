// What a function's lowering decides once, at its entry: the values every
// inline path shares (hoisted), and which of its blocks run once
// (IlLowering::run_once).

#include "il_lowering.h"
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

namespace {

// The IL successors of `blk`: its branch targets and its handler.
template <typename F>
void for_each_successor(const BronzeBlock& blk, F&& f) {
    for (const auto& inst : blk.instructions) {
        if (inst.op == BronzeOp::Jump) f(inst.target.block_id);
        if (inst.op == BronzeOp::Branch) {
            f(inst.target.block_id);
            f(inst.else_target.block_id);
        }
    }
    if (blk.handler_id != UINT32_MAX) f(blk.handler_id);
}

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

bool is_pure_or_proven_read(const BronzeInstruction& inst,
                            const std::unordered_map<uint32_t, uint32_t>& proofs,
                            const PropertyLoweringHelper& prop_lowering) {
    if (is_pure_scalar_inst(inst) || inst.op == BronzeOp::IsDenseArray || inst.op == BronzeOp::MathUnary) {
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
        case BronzeOp::BitAnd:
        case BronzeOp::BitOr:
        case BronzeOp::BitXor:
        case BronzeOp::Shl:
        case BronzeOp::Shr:
        case BronzeOp::UShr:
        case BronzeOp::BitNot:
        case BronzeOp::Sub:
        case BronzeOp::Mul:
        case BronzeOp::Div:
        case BronzeOp::Mod:
        case BronzeOp::Pow:
        case BronzeOp::Neg:
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
    fresh_objects_.clear();
    hole_insensitive_ids_.clear();
    unrooted_hole_insensitive_ids_.clear();
    primitive_value_ids_.clear();
    current_fn_dynamic_ids_.clear();
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
    if (!fn_ast.is_toplevel || fn_ast.blocks.empty()) return;

    // The blocks on no cycle of the CFG: its strongly connected components
    // of one block without a self edge (Tarjan's, iteratively — a top level
    // has thousands of blocks).
    const size_t n = fn_ast.blocks.size();
    std::unordered_map<uint32_t, uint32_t> index_of;
    index_of.reserve(n);
    for (size_t i = 0; i < n; ++i) index_of.emplace(fn_ast.blocks[i].id, static_cast<uint32_t>(i));
    std::vector<std::vector<uint32_t>> succ(n);
    std::vector<uint8_t> self_edge(n, 0);
    for (size_t i = 0; i < n; ++i) {
        for_each_successor(fn_ast.blocks[i], [&](uint32_t id) {
            const auto it = index_of.find(id);
            if (it == index_of.end()) return;
            succ[i].push_back(it->second);
            if (it->second == i) self_edge[i] = 1;
        });
    }
    constexpr uint32_t kUnvisited = UINT32_MAX;
    std::vector<uint32_t> order(n, kUnvisited), low(n, 0);
    std::vector<uint8_t> on_stack(n, 0);
    std::vector<uint32_t> stack;
    std::vector<std::pair<uint32_t, size_t>> work;
    uint32_t counter = 0;
    for (uint32_t root = 0; root < n; ++root) {
        if (order[root] != kUnvisited) continue;
        work.emplace_back(root, 0);
        while (!work.empty()) {
            auto& [v, next] = work.back();
            if (next == 0 && order[v] == kUnvisited) {
                order[v] = low[v] = counter++;
                stack.push_back(v);
                on_stack[v] = 1;
            }
            if (next < succ[v].size()) {
                const uint32_t w = succ[v][next++];
                if (order[w] == kUnvisited) {
                    work.emplace_back(w, 0);
                } else if (on_stack[w]) {
                    low[v] = std::min(low[v], order[w]);
                }
                continue;
            }
            if (low[v] == order[v]) {
                size_t members = 0;
                uint32_t w = 0;
                do {
                    w = stack.back();
                    stack.pop_back();
                    on_stack[w] = 0;
                    ++members;
                } while (w != v);
                if (members == 1 && !self_edge[v]) run_once_blocks_.insert(fn_ast.blocks[v].id);
            }
            const uint32_t done = v;
            work.pop_back();
            if (!work.empty()) {
                const uint32_t parent = work.back().first;
                low[parent] = std::min(low[parent], low[done]);
            }
        }
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

bool IlLowering::try_lower_array_index_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                           std::unordered_map<uint32_t, Value*>& val_map,
                                           const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                           uint32_t* cont_counter) {
    if (run_once_ || !prop_lowering_.enable_inlined_fastpaths() || !prop_lowering_.inline_sets()) {
        return false;
    }

    auto parse_prop_index = [&](const BronzeInstruction& inst, uint32_t& out_idx) -> bool {
        if (inst.index == PropertyLoweringHelper::kNoKey || inst.static_slot != UINT32_MAX) return false;
        const auto parsed = bronze::il::parseIndexKey(prop_lowering_.key_tag(inst.index));
        if (!parsed) return false;
        out_idx = *parsed;
        return true;
    };

    const BronzeInstruction& first = blk_ast.instructions[inst_idx];
    uint32_t first_idx = 0;
    const bool first_is_set =
        first.op == BronzeOp::PropSet && first.operands.size() >= 2 && parse_prop_index(first, first_idx);
    const bool first_is_get =
        first.op == BronzeOp::PropGet && !first.operands.empty() && first.result_id != UINT32_MAX &&
        parse_prop_index(first, first_idx);
    const bool first_is_scalar = is_pure_scalar_inst(first);
    if (!first_is_set && !first_is_get && !first_is_scalar) return false;

    auto apply_loc = [&](const BronzeInstruction& inst) {
        if (current_file_id_ != 0 && inst.line > 0) {
            const uint32_t fileId = inst.file < source_file_ids_.size() ? source_file_ids_[inst.file] : current_file_id_;
            b.set_current_loc(DebugLoc(fileId, inst.line, inst.column));
        } else {
            b.clear_current_loc();
        }
    };

    // Contiguous PropGet runs across one or more receivers (e.g. `ae[0..15]` + `be[0..15]`)
    // when not already proven dense and not a single-use store source.
    if (first_is_get && single_block_propset_use_.count(first.result_id) == 0 &&
        !is_proven_dense_array(first.operands[0], first_idx)) {
        struct RecvGroup { uint32_t recv_id; uint32_t max_idx; size_t count; Value* block = nullptr; Value* head = nullptr; };
        std::vector<RecvGroup> groups;
        size_t run_end = inst_idx;
        while (run_end < blk_ast.instructions.size()) {
            const BronzeInstruction& inst = blk_ast.instructions[run_end];
            uint32_t elem_idx = 0;
            if (inst.op != BronzeOp::PropGet || inst.operands.empty() || inst.result_id == UINT32_MAX ||
                single_block_propset_use_.count(inst.result_id) != 0 || !parse_prop_index(inst, elem_idx) ||
                is_proven_dense_array(inst.operands[0], elem_idx)) {
                break;
            }
            const uint32_t rid = inst.operands[0];
            if (!groups.empty() && groups.back().recv_id == rid) {
                groups.back().max_idx = std::max(groups.back().max_idx, elem_idx);
                ++groups.back().count;
            } else if (groups.empty() || groups.back().count >= 2) {
                groups.push_back(RecvGroup{rid, elem_idx, 1});
            } else {
                break;
            }
            ++run_end;
        }
        if (!groups.empty() && groups.back().count < 2) {
            run_end -= groups.back().count;
            groups.pop_back();
        }
        const size_t get_count = run_end - inst_idx;
        if (get_count >= 2) {
            apply_loc(first);
            Value* all_ok = nullptr;
            std::unordered_map<uint32_t, std::pair<Value*, Value*>> recv_cursors;
            for (RecvGroup& g : groups) {
                if (auto it = recv_cursors.find(g.recv_id); it != recv_cursors.end()) {
                    g.block = it->second.first;
                    g.head = it->second.second;
                    continue;
                }
                Value* obj = ensure_type(get_val_by_id(g.recv_id, b, val_map), Type::i64(), b);
                uint32_t max_for_recv = g.max_idx;
                for (const RecvGroup& o : groups) {
                    if (o.recv_id == g.recv_id) max_for_recv = std::max(max_for_recv, o.max_idx);
                }
                Value* ok = nullptr;
                prop_lowering_.emit_dense_array_guard(b, obj, max_for_recv, /*writable=*/false, ok, g.block, g.head);
                all_ok = all_ok ? b.build_and(all_ok, ok) : ok;
                recv_cursors[g.recv_id] = {g.block, g.head};
            }

            BasicBlock* guard_bb = b.current_block();
            const std::string prefix = "prop_idx_get_run_" + std::to_string(fn->next_block_id());
            BasicBlock* bb_fast = b.append_block(prefix + "_fast");
            BasicBlock* bb_slow = b.append_block(prefix + "_slow");
            BasicBlock* bb_merge = b.append_block(prefix + "_merge");

            std::vector<Value*> merge_params;
            merge_params.reserve(get_count);
            for (size_t j = inst_idx; j < run_end; ++j) {
                const uint32_t res_id = blk_ast.instructions[j].result_id;
                const Type ptype = current_fn_dynamic_ids_.count(res_id) ? Type::tagged() : Type::i64();
                merge_params.push_back(b.add_block_param(bb_merge, ptype));
            }

            b.position_at_end(guard_bb);
            b.build_br_if(all_ok, bb_fast, bb_slow);

            b.position_at_end(bb_fast);
            std::vector<Value*> fast_args;
            fast_args.reserve(get_count);
            for (size_t j = inst_idx; j < run_end; ++j) {
                const BronzeInstruction& inst = blk_ast.instructions[j];
                uint32_t elem_idx = 0;
                parse_prop_index(inst, elem_idx);
                apply_loc(inst);
                const auto [block, head] = recv_cursors[inst.operands[0]];
                const bool check_hole = !is_hole_insensitive(inst.result_id);
                Value* v = prop_lowering_.load_proven_dense_array_elem(b, block, head, elem_idx, check_hole);
                fast_args.push_back(ensure_type(v, merge_params[j - inst_idx]->type(), b));
            }
            b.build_br(bb_merge, fast_args);

            b.position_at_end(bb_slow);
            std::vector<Value*> slow_args;
            slow_args.reserve(get_count);
            for (size_t j = inst_idx; j < run_end; ++j) {
                const BronzeInstruction& inst = blk_ast.instructions[j];
                apply_loc(inst);
                Value* recv = ensure_type(get_val_by_id(inst.operands[0], b, val_map), Type::i64(), b);
                const IcSite site = prop_lowering_.ic_site_ref(b, inst.ic_index);
                Value* v = prop_lowering_.lower_prop_get(b, recv, inst.index,
                                                         site ? prop_lowering_.site_ptr(b, site) : nullptr);
                slow_args.push_back(ensure_type(v, merge_params[j - inst_idx]->type(), b));
            }
            b.build_br(bb_merge, slow_args);

            b.position_at_end(bb_merge);
            for (size_t j = inst_idx; j < run_end; ++j) {
                val_map[blk_ast.instructions[j].result_id] = merge_params[j - inst_idx];
            }
            invalidate_dense_array_proofs();
            inst_idx = run_end - 1;
            return true;
        }
        return false;
    }

    uint32_t set_recv_id = UINT32_MAX;
    uint32_t set_max_idx = 0;
    uint32_t get_recv_id = UINT32_MAX;
    uint32_t get_max_idx = 0;
    size_t set_count = 0;
    size_t max_required_set_j = inst_idx;
    std::unordered_set<uint32_t> get_results;

    size_t committed_end = inst_idx;
    size_t committed_set_count = 0;
    uint32_t committed_set_max_idx = 0;
    uint32_t committed_get_recv_id = UINT32_MAX;
    uint32_t committed_get_max_idx = 0;

    for (size_t j = inst_idx; j < blk_ast.instructions.size(); ++j) {
        const BronzeInstruction& inst = blk_ast.instructions[j];
        uint32_t elem_idx = 0;
        if (inst.op == BronzeOp::PropSet && inst.operands.size() >= 2 && parse_prop_index(inst, elem_idx)) {
            const uint32_t recv = inst.operands[0];
            if (set_recv_id == UINT32_MAX) {
                set_recv_id = recv;
                set_max_idx = elem_idx;
            } else if (recv == set_recv_id) {
                set_max_idx = std::max(set_max_idx, elem_idx);
            } else {
                break;
            }
            ++set_count;
            if (j >= max_required_set_j) {
                committed_end = j + 1;
                committed_set_count = set_count;
                committed_set_max_idx = set_max_idx;
                committed_get_recv_id = get_recv_id;
                committed_get_max_idx = get_max_idx;
            }
            continue;
        }
        if (inst.op == BronzeOp::PropGet && !inst.operands.empty() && inst.result_id != UINT32_MAX &&
            parse_prop_index(inst, elem_idx)) {
            const auto use_it = single_block_propset_use_.find(inst.result_id);
            if (use_it == single_block_propset_use_.end()) break;
            const uint32_t recv = inst.operands[0];
            if (get_recv_id == UINT32_MAX) {
                get_recv_id = recv;
                get_max_idx = elem_idx;
            } else if (recv == get_recv_id) {
                get_max_idx = std::max(get_max_idx, elem_idx);
            } else {
                break;
            }
            get_results.insert(inst.result_id);
            max_required_set_j = std::max(max_required_set_j, static_cast<size_t>(use_it->second));
            continue;
        }
        if (is_pure_scalar_inst(inst)) {
            bool uses_get = false;
            for (uint32_t op_id : inst.operands) {
                if (get_results.count(op_id) != 0) {
                    uses_get = true;
                    break;
                }
            }
            if (uses_get) break;
            continue;
        }
        break;
    }

    if (committed_set_count < 2 || set_recv_id == UINT32_MAX) {
        return false;
    }

    // Identify pure scalar instructions whose results are used exclusively inside [inst_idx, committed_end).
    // Lowering those inside bb_fast and bb_slow (instead of hoisting all of them into guard_bb) keeps
    // their SSA values local to bb_fast (which has zero calls) and avoids live-across-call spills.
    std::unordered_map<uint32_t, uint32_t> uses_in_run;
    for (size_t j = inst_idx; j < committed_end; ++j) {
        for (uint32_t op_id : blk_ast.instructions[j].operands) ++uses_in_run[op_id];
    }
    auto is_run_local_scalar = [&](const BronzeInstruction& inst) {
        if (inst.op == BronzeOp::PropGet || inst.op == BronzeOp::PropSet || inst.result_id == UINT32_MAX) {
            return false;
        }
        if (inst.result_id == set_recv_id || inst.result_id == committed_get_recv_id) return false;
        const auto vit = value_use_counts_.find(inst.result_id);
        const auto rit = uses_in_run.find(inst.result_id);
        return vit != value_use_counts_.end() && rit != uses_in_run.end() && vit->second == rit->second;
    };

    // 1. Lower only non-local pure scalar computations in the current block before the guard.
    for (size_t j = inst_idx; j < committed_end; ++j) {
        const BronzeInstruction& inst = blk_ast.instructions[j];
        if (inst.op == BronzeOp::PropGet || inst.op == BronzeOp::PropSet || is_run_local_scalar(inst)) continue;
        apply_loc(inst);
        if (!lower_instruction(inst, b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
            return false;
        }
    }

    // 2. Emit combined dense-array guard(s) for the run.
    apply_loc(blk_ast.instructions[inst_idx]);
    Value* set_obj = ensure_type(get_val_by_id(set_recv_id, b, val_map), Type::i64(), b);
    Value* set_ok = nullptr;
    Value* set_block = nullptr;
    Value* set_head = nullptr;
    prop_lowering_.emit_dense_array_guard(b, set_obj, committed_set_max_idx, /*writable=*/true,
                                          set_ok, set_block, set_head);
    Value* all_ok = set_ok;
    Value* get_block = nullptr;
    Value* get_head = nullptr;
    if (committed_get_recv_id != UINT32_MAX) {
        if (committed_get_recv_id == set_recv_id) {
            get_block = set_block;
            get_head = set_head;
        } else {
            Value* get_obj = ensure_type(get_val_by_id(committed_get_recv_id, b, val_map), Type::i64(), b);
            if (is_proven_dense_array(committed_get_recv_id, committed_get_max_idx)) {
                get_dense_array_cursor(committed_get_recv_id, get_obj, b, get_block, get_head);
            } else {
                Value* get_ok = nullptr;
                prop_lowering_.emit_dense_array_guard(b, get_obj, committed_get_max_idx, /*writable=*/false,
                                                      get_ok, get_block, get_head);
                all_ok = b.build_and(all_ok, get_ok);
            }
        }
    }

    BasicBlock* guard_bb = b.current_block();
    const std::string prefix = "prop_idx_run_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(guard_bb);
    b.build_br_if(all_ok, bb_fast, bb_slow);

    std::unordered_map<uint32_t, Value*> slow_val_map = val_map;

    // 3. Fast path: straight-line indexed loads, run-local scalars, and stores.
    b.position_at_end(bb_fast);
    std::unordered_map<uint32_t, Value*> fast_get_vals;
    std::vector<Value*> non_prim_vals;
    for (size_t j = inst_idx; j < committed_end; ++j) {
        const BronzeInstruction& inst = blk_ast.instructions[j];
        uint32_t elem_idx = 0;
        if (inst.op == BronzeOp::PropGet) {
            parse_prop_index(inst, elem_idx);
            apply_loc(inst);
            const bool check_hole = !is_hole_insensitive(inst.result_id);
            fast_get_vals[inst.result_id] =
                prop_lowering_.load_proven_dense_array_elem(b, get_block, get_head, elem_idx, check_hole);
        } else if (inst.op == BronzeOp::PropSet) {
            parse_prop_index(inst, elem_idx);
            apply_loc(inst);
            const uint32_t val_id = inst.operands[1];
            Value* val = nullptr;
            if (const auto it = fast_get_vals.find(val_id); it != fast_get_vals.end()) {
                val = it->second;
            } else {
                val = ensure_type(get_val_by_id(val_id, b, val_map), Type::i64(), b);
            }
            prop_lowering_.store_proven_dense_array_elem(b, set_block, set_head, elem_idx, val);
            if (!is_primitive_value(val_id) && !prop_lowering_.is_known_non_ref(val)) {
                non_prim_vals.push_back(val);
            }
        } else if (is_run_local_scalar(inst)) {
            apply_loc(inst);
            if (!lower_instruction(inst, b, fn, val_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
                return false;
            }
        }
    }
    if (non_prim_vals.empty()) {
        b.build_br(bb_merge);
    } else {
        BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
        b.position_at_end(bb_fast);
        Value* max_num = b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits));
        Value* any_ref = b.build_ugt(non_prim_vals[0], max_num);
        for (size_t k = 1; k < non_prim_vals.size(); ++k) {
            any_ref = b.build_or(any_ref, b.build_ugt(non_prim_vals[k], max_num));
        }
        b.build_br_if(any_ref, bb_barrier, bb_merge);
        b.position_at_end(bb_barrier);
        for (Value* v : non_prim_vals) {
            b.build_call("brass_gc_write_barrier", Type::void_type(), {set_block, v});
        }
        b.build_br(bb_merge);
    }

    // 4. Slow path: runtime helper calls in program order.
    b.position_at_end(bb_slow);
    std::unordered_map<uint32_t, Value*> slow_get_vals;
    for (size_t j = inst_idx; j < committed_end; ++j) {
        const BronzeInstruction& inst = blk_ast.instructions[j];
        if (inst.op == BronzeOp::PropGet) {
            apply_loc(inst);
            Value* recv = ensure_type(get_val_by_id(inst.operands[0], b, slow_val_map), Type::i64(), b);
            const IcSite site = prop_lowering_.ic_site_ref(b, inst.ic_index);
            Value* slow_val = prop_lowering_.lower_prop_get(b, recv, inst.index,
                                                            site ? prop_lowering_.site_ptr(b, site) : nullptr);
            slow_get_vals[inst.result_id] = ensure_type(slow_val, Type::tagged(), b);
        } else if (inst.op == BronzeOp::PropSet) {
            apply_loc(inst);
            Value* recv = ensure_type(get_val_by_id(inst.operands[0], b, slow_val_map), Type::i64(), b);
            const uint32_t val_id = inst.operands[1];
            Value* val = nullptr;
            if (const auto it = slow_get_vals.find(val_id); it != slow_get_vals.end()) {
                val = ensure_type(it->second, Type::i64(), b);
            } else {
                val = ensure_type(get_val_by_id(val_id, b, slow_val_map), Type::i64(), b);
            }
            const IcSite site = prop_lowering_.ic_site_ref(b, inst.ic_index);
            const uint32_t strict = static_cast<uint32_t>(inst.imm_i64);
            prop_lowering_.lower_prop_set(b, recv, inst.index, val, strict,
                                          site ? prop_lowering_.site_ptr(b, site) : nullptr);
        } else if (is_run_local_scalar(inst)) {
            apply_loc(inst);
            if (!lower_instruction(inst, b, fn, slow_val_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
                return false;
            }
        }
    }
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
    invalidate_dense_array_proofs();
    inst_idx = committed_end - 1;
    return true;
}

} // namespace il2mir
