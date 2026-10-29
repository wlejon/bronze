// CONSTANT-INDEX RUNS: a stretch of `o[k]` reads and writes over constant
// keys, on one Array receiver per side, behind one dense-array guard instead
// of one per access (IlLowering::try_lower_array_index_run). Split from
// il_lowering_hoist.cpp, whose per-function analysis decides which reads may
// join a run (`single_block_propset_use_`).

#include "il_lowering.h"
#include "il_property_lowering.h"
#include "il/key.h"
#include "il_abi.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il2mir {
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

    auto apply_loc = [&](const BronzeInstruction& inst) { apply_inst_loc(b, inst); };

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

            // A read il.h `numberBits` licenses merges as a double (number_bits_ids_).
            std::vector<Value*> merge_params;
            merge_params.reserve(get_count);
            for (size_t j = inst_idx; j < run_end; ++j) {
                const BronzeInstruction& inst = blk_ast.instructions[j];
                const Type ptype = inst.number_bits                               ? Type::f64()
                                   : current_fn_dynamic_ids_.count(inst.result_id) ? Type::tagged()
                                                                                   : Type::i64();
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
                Value* v = prop_lowering_.load_proven_dense_array_elem(b, block, head, elem_idx, check_hole,
                                                                       inst.number_bits ? Type::f64() : Type::i64());
                fast_args.push_back(inst.number_bits ? v : ensure_type(v, merge_params[j - inst_idx]->type(), b));
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
                slow_args.push_back(ensure_type(v, inst.number_bits ? Type::tagged() : merge_params[j - inst_idx]->type(), b));
            }
            // The helper reads are GC points, so a double-held read's bits
            // become a double only after the last of them: each stays a
            // tagged, rooted value until no collection can follow it.
            for (size_t j = inst_idx; j < run_end; ++j) {
                if (blk_ast.instructions[j].number_bits) {
                    Value*& arg = slow_args[j - inst_idx];
                    arg = b.build_bitcast_f64_i64(ensure_type(arg, Type::i64(), b));
                }
            }
            b.build_br(bb_merge, slow_args);

            b.position_at_end(bb_merge);
            for (size_t j = inst_idx; j < run_end; ++j) {
                const BronzeInstruction& inst = blk_ast.instructions[j];
                val_map[inst.result_id] = merge_params[j - inst_idx];
                if (inst.number_bits) number_bits_ids_[inst.result_id] = !is_hole_insensitive(inst.result_id);
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

    // STATIC READS AS MEMBERS. `te[12] = position.x; te[13] = position.y; ...`
    // (Matrix4.compose) is a store run with a read of a plain object's static
    // slot (il_static_slot.cpp) in front of each store, and lowered apart
    // each read pays its receiver's shape guard and each store the array's
    // dense guard again. A static read whose one use is a store of the run
    // joins it instead: its guard term is ANDed into the run's guard, and on
    // the fast path it is the slot load the hit would have been. Nothing
    // between the reads and the stores runs code — the reads are loads of an
    // own data slot the shape guard vouches for, the stores land in bounds of
    // a dense block the array guard vouches for — and the two receivers are
    // distinct objects, since one guard admits only a plain object and the
    // other only an Array. The slow path is the stretch in program order with
    // each read lowered as itself (guard, cache, fill), so a getter runs
    // where the program runs it.
    uint32_t sget_recv_id = UINT32_MAX;
    uint32_t committed_sget_recv_id = UINT32_MAX;
    auto static_read_use = [&](const BronzeInstruction& inst, size_t j) -> size_t {
        if (inst.op != BronzeOp::PropGet || inst.static_slot == BronzeInstruction::kNoStaticSlot ||
            inst.operands.empty() || inst.result_id == UINT32_MAX || inst.number_bits) {
            return SIZE_MAX;
        }
        if (prop_lowering_.key_tag(inst.index) == "length" || !static_get_lowerable(this, inst)) return SIZE_MAX;
        const auto uit = value_use_counts_.find(inst.result_id);
        if (uit == value_use_counts_.end() || uit->second != 1) return SIZE_MAX;
        for (size_t k = j + 1; k < blk_ast.instructions.size(); ++k) {
            const BronzeInstruction& use = blk_ast.instructions[k];
            const auto& ops = use.operands;
            if (std::find(ops.begin(), ops.end(), inst.result_id) == ops.end()) continue;
            uint32_t idx = 0;
            const bool stored = use.op == BronzeOp::PropSet && ops.size() >= 2 && ops[1] == inst.result_id &&
                                ops[0] != inst.result_id && parse_prop_index(use, idx);
            return stored ? k : SIZE_MAX;
        }
        return SIZE_MAX;
    };

    for (size_t j = inst_idx; j < blk_ast.instructions.size(); ++j) {
        const BronzeInstruction& inst = blk_ast.instructions[j];
        uint32_t elem_idx = 0;
        if (inst.op == BronzeOp::PropSet && inst.operands.size() >= 2 && parse_prop_index(inst, elem_idx)) {
            const uint32_t recv = inst.operands[0];
            if (set_recv_id == UINT32_MAX) {
                if (recv == sget_recv_id) break;
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
                committed_sget_recv_id = sget_recv_id;
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
        if (const size_t use_j = static_read_use(inst, j); use_j != SIZE_MAX) {
            const uint32_t recv = inst.operands[0];
            if ((sget_recv_id != UINT32_MAX && recv != sget_recv_id) || recv == set_recv_id) break;
            // The store's receiver must not be this read's either; the scan
            // ends there when it is (the store is not a member).
            if (blk_ast.instructions[use_j].operands[0] == recv) break;
            sget_recv_id = recv;
            get_results.insert(inst.result_id);
            max_required_set_j = std::max(max_required_set_j, use_j);
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

    Value* sget_ptr = nullptr;
    if (committed_sget_recv_id != UINT32_MAX) {
        std::vector<StaticGuardTerm> terms;
        for (size_t j = inst_idx; j < committed_end; ++j) {
            const BronzeInstruction& inst = blk_ast.instructions[j];
            if (inst.op == BronzeOp::PropGet && inst.static_slot != BronzeInstruction::kNoStaticSlot) {
                terms.push_back(StaticGuardTerm{StaticSlotSite::of(inst)});
            }
        }
        Value* sget_obj = ensure_type(get_val_by_id(committed_sget_recv_id, b, val_map), Type::i64(), b);
        all_ok = b.build_and(all_ok, prop_lowering_.static_guard(b, sget_obj,
                                                                 memo_keyed(b, committed_sget_recv_id, sget_obj),
                                                                 terms, sget_ptr));
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
        if (inst.op == BronzeOp::PropGet && inst.static_slot != BronzeInstruction::kNoStaticSlot) {
            apply_loc(inst);
            int32_t off = 0;
            Value* base = prop_lowering_.static_slot_base(b, sget_ptr, inst.static_slot, off);
            fast_get_vals[inst.result_id] = b.build_load(Type::i64(), base, off);
        } else if (inst.op == BronzeOp::PropGet) {
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
            } else if (!prop_lowering_.is_known_number(val)) {
                // il_number_array.cpp, the rule for a store.
                prop_lowering_.forget_numbers_unless_number(b, set_obj, val);
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
        prop_lowering_.forget_numbers(b, set_obj);
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
        if (inst.op == BronzeOp::PropGet && inst.static_slot != BronzeInstruction::kNoStaticSlot) {
            // The read as itself, so a cell that was never published is
            // published here and the next entry takes the fast path.
            apply_loc(inst);
            if (!lower_instruction(inst, b, fn, slow_val_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
                return false;
            }
            slow_get_vals[inst.result_id] = ensure_type(slow_val_map.at(inst.result_id), Type::tagged(), b);
        } else if (inst.op == BronzeOp::PropGet) {
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
