// STATIC RUNS: one guard for a stretch of static-slot sites on one receiver.
//
// `node.worldX = px + node.x * node.weight; node.worldY = ...` is seven sites
// on one SSA receiver with nothing but arithmetic between them, and lowered
// site by site each one asks again whether `node` is an object, a plain one,
// and one of the shapes its guard admits (il_static_slot.cpp). Nothing between
// two of them can change the answer: the receiver is one SSA value, so its tag
// is fixed, and its shape changes only by a transition, which nothing in such
// a stretch can perform — a hit stores into a slot the shape already has, and
// arithmetic on numbers runs no user code. So the questions are asked once, as
// one i1 over every site's guard, and the stretch is lowered twice behind it:
//
//   fast   every read a load and every write a store at its constant offset,
//          the arithmetic between them as itself;
//   slow   the stretch exactly as it would have been lowered without this,
//          each site with its own guard, cache and fill.
//
// The slow copy is the whole of the miss, so a receiver of any other shape
// gets what it got before and a layout that was wrong still costs only time.
// What may stand in a stretch is decided here and nowhere else: a member is a
// static site on the receiver, or an instruction that neither calls nor
// allocates nor can throw. A write whose value the guard cannot test up front
// (its representation question needs the value, and the value is computed
// inside the stretch, and is not known to be a Number) ends it.

#include "il_abi.h"
#include "il_lowering.h"
#include "il_property.h"
#include "il_property_lowering.h"
#include <brass/mir/function.hpp>
#include <unordered_set>

namespace il2mir {

namespace {

// An instruction the fast copy can carry between two sites: no call, no
// allocation, no throw, nothing that reads or writes the heap. The list is
// written out rather than borrowed, so that an op added tomorrow is not a
// member until someone says it may be.
bool runs_nothing(const BronzeInstruction& inst) {
    const auto scalar = [&] {
        return inst.result_type == BronzeType::F64 || inst.result_type == BronzeType::I32 ||
               inst.result_type == BronzeType::Bool;
    };
    switch (inst.op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
        case BronzeOp::IsNumber:
        case BronzeOp::NumTruthy:
            return true;
        case BronzeOp::Box:
            return inst.box_type == BronzeType::F64 || inst.box_type == BronzeType::I32 ||
                   inst.box_type == BronzeType::Bool;
        // Only the unboxes whose proof makes them bit operations: a checked
        // one is ToNumber, which can call `valueOf`.
        case BronzeOp::Unbox:
            return inst.result_type == BronzeType::F64 && (inst.raw_unbox || inst.nullish_unbox);
        case BronzeOp::Add:
        case BronzeOp::Sub:
        case BronzeOp::Mul:
        case BronzeOp::Div:
        case BronzeOp::Neg:
        case BronzeOp::CmpLt:
        case BronzeOp::CmpLe:
        case BronzeOp::CmpGt:
        case BronzeOp::CmpGe:
        case BronzeOp::CmpEq:
        case BronzeOp::CmpNe:
            return scalar();
        default:
            return false;
    }
}

} // namespace

bool IlLowering::try_lower_static_slot_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                           std::unordered_map<uint32_t, Value*>& val_map,
                                           const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                           uint32_t* cont_counter) {
    const auto& insts = blk_ast.instructions;
    const BronzeInstruction& first = insts[inst_idx];
    if (first.op != BronzeOp::PropGet && first.op != BronzeOp::PropSet) return false;
    if (first.static_slot == BronzeInstruction::kNoStaticSlot || first.operands.empty()) return false;
    const uint32_t recv_id = first.operands[0];

    // A site of the stretch: static, on the receiver, lowered through its
    // guard here. `length` keeps its own path, which a preloaded typed-array
    // cursor answers before any guard.
    auto is_site = [&](const BronzeInstruction& inst) {
        if (inst.op != BronzeOp::PropGet && inst.op != BronzeOp::PropSet) return false;
        if (inst.static_slot == BronzeInstruction::kNoStaticSlot || inst.operands.empty()) return false;
        if (inst.operands[0] != recv_id) return false;
        if (prop_lowering_.key_tag(inst.index) == "length") return false;
        return inst.op == BronzeOp::PropGet ? inst.result_id != UINT32_MAX && static_get_lowerable(this, inst)
                                            : static_set_lowerable(this, inst);
    };
    if (!is_site(first)) return false;

    std::unordered_set<uint32_t> defined_in_run;
    size_t end = inst_idx;      // one past the last site taken
    size_t sites = 0;
    for (size_t j = inst_idx; j < insts.size(); ++j) {
        const BronzeInstruction& inst = insts[j];
        if (is_site(inst)) {
            // The guard asks a write's representation question before the
            // stretch runs, so it must hold the value then.
            if (inst.op == BronzeOp::PropSet && defined_in_run.count(inst.operands[1]) &&
                !is_number_value(inst.operands[1])) {
                break;
            }
            ++sites;
            end = j + 1;
        } else if (!runs_nothing(inst)) {
            break;
        }
        // The receiver is never redefined inside (SSA), but a member that
        // named it as its result would be a malformed block; refuse it.
        if (inst.result_id == recv_id) return false;
        if (inst.result_id != UINT32_MAX) defined_in_run.insert(inst.result_id);
    }
    if (sites < 2) return false;

    // What leaves the stretch: a value defined in it with a use past it. Each
    // is one parameter of the join, fed by both copies.
    std::unordered_map<uint32_t, uint32_t> uses_in_run;
    for (size_t j = inst_idx; j < end; ++j) {
        for (uint32_t op_id : insts[j].operands) ++uses_in_run[op_id];
    }
    std::vector<uint32_t> live_out;
    for (size_t j = inst_idx; j < end; ++j) {
        const uint32_t id = insts[j].result_id;
        if (id == UINT32_MAX) continue;
        const auto vit = value_use_counts_.find(id);
        const uint32_t total = vit == value_use_counts_.end() ? 0 : vit->second;
        const auto rit = uses_in_run.find(id);
        if (total > (rit == uses_in_run.end() ? 0 : rit->second)) live_out.push_back(id);
    }

    apply_inst_loc(b, first);
    Value* obj = ensure_type(get_val_by_id(recv_id, b, val_map), Type::i64(), b);
    std::vector<StaticGuardTerm> terms;
    for (size_t j = inst_idx; j < end; ++j) {
        const BronzeInstruction& inst = insts[j];
        if (!is_site(inst)) continue;
        StaticGuardTerm t{StaticSlotSite::of(inst)};
        if (inst.op == BronzeOp::PropSet) {
            t.store_is_number = is_number_value(inst.operands[1]);
            // A value from inside the stretch is a Number (the scan stopped
            // short of any other), and asks nothing.
            if (!t.store_is_number) t.store = ensure_type(get_val_by_id(inst.operands[1], b, val_map), Type::i64(), b);
        }
        terms.push_back(t);
    }
    Value* ptr = nullptr;
    Value* ok = prop_lowering_.static_guard(b, obj, memo_keyed(b, recv_id, obj), terms, ptr);
    // The shape memo (il_shape_memo.cpp) as the fast copy leaves it: nothing
    // in the fast copy moves a shape.
    const auto memo_fast = memo_snapshot();

    BasicBlock* guard_bb = b.current_block();
    const std::string prefix = "static_run_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    // Named cold for brass's block layout, as a lone site's miss is
    // (il_static_slot.cpp): the slow copy is its own trace.
    BasicBlock* bb_slow = b.append_block(prefix + "_slow_cold");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    b.position_at_end(guard_bb);
    b.build_br_if(ok, bb_fast, bb_slow);

    std::unordered_map<uint32_t, Value*> fast_map = val_map;
    std::unordered_map<uint32_t, Value*> slow_map = val_map;

    b.position_at_end(bb_fast);
    for (size_t j = inst_idx; j < end; ++j) {
        const BronzeInstruction& inst = insts[j];
        apply_inst_loc(b, inst);
        if (!is_site(inst)) {
            if (!lower_instruction(inst, b, fn, fast_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
                return false;
            }
            continue;
        }
        int32_t off = 0;
        Value* base = prop_lowering_.static_slot_base(b, ptr, inst.static_slot, off);
        if (inst.op == BronzeOp::PropGet) {
            set_inst_result(inst.result_id, b.build_load(Type::i64(), base, off), b, fast_map);
            continue;
        }
        const uint32_t val_id = inst.operands[1];
        Value* val = ensure_type(get_val_by_id(val_id, b, fast_map), Type::i64(), b);
        const bool need_barrier = !is_primitive_value(val_id) && !prop_lowering_.is_known_non_ref(val);
        prop_lowering_.store_with_barrier(b, base, off, val, need_barrier, prefix);
        if (need_barrier) {
            // The barrier is a call, so a collection may have moved the
            // receiver: its payload is read again from the rooted value. The
            // guard's tag test still holds — a move changes where the object
            // is, never what it is.
            Value* bits = ensure_type(get_val_by_id(recv_id, b, fast_map), Type::i64(), b);
            ptr = b.build_xor(bits, b.build_iconst_i64(static_cast<int64_t>(uint64_t{BRONZE_ABI_TAG_OBJECT} << 48)));
        }
    }
    std::vector<Value*> fast_args;
    for (uint32_t id : live_out) fast_args.push_back(fast_map.at(id));
    BasicBlock* fast_end = b.current_block();

    b.position_at_end(bb_slow);
    for (size_t j = inst_idx; j < end; ++j) {
        const BronzeInstruction& inst = insts[j];
        apply_inst_loc(b, inst);
        if (!lower_instruction(inst, b, fn, slow_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
            return false;
        }
    }
    std::vector<Value*> slow_args;
    for (size_t k = 0; k < live_out.size(); ++k) {
        slow_args.push_back(ensure_type(slow_map.at(live_out[k]), fast_args[k]->type(), b));
    }
    BasicBlock* slow_end = b.current_block();

    // The memo past the join: a receiver both copies still hold, as one value
    // when they agree and else as a join parameter fed by each. The slow
    // copy's sites carried it across their own joins, so what it holds at
    // its end is true there.
    const auto memo_slow = memo_snapshot();
    std::vector<std::pair<uint32_t, Value*>> memo_join;
    std::vector<size_t> memo_params;
    for (const auto& [id, fv] : memo_fast) {
        for (const auto& [sid, sv] : memo_slow) {
            if (sid != id) continue;
            if (sv != fv) {
                memo_params.push_back(memo_join.size());
                fast_args.push_back(fv);
                slow_args.push_back(sv);
            }
            memo_join.emplace_back(id, fv);
            break;
        }
    }

    for (size_t k = 0; k < live_out.size(); ++k) {
        val_map[live_out[k]] = b.add_block_param(bb_merge, fast_args[k]->type());
    }
    for (size_t idx : memo_params) memo_join[idx].second = b.add_block_param(bb_merge, Type::i64());
    memo_restore(std::move(memo_join));
    b.position_at_end(slow_end);
    b.build_br(bb_merge, slow_args);
    b.position_at_end(fast_end);
    b.build_br(bb_merge, fast_args);
    b.position_at_end(bb_merge);
    // The slow copy runs the cache and its helpers, which may run setters.
    invalidate_dense_array_proofs();
    inst_idx = end - 1;
    return true;
}

} // namespace il2mir
