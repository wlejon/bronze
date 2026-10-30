// TRANSITION RUNS: a stretch of writes that adds keys to one receiver, lowered
// behind one guard (lower/transition_runs.h marks them; il.h
// `transitionRunCell` names the cells; runtime/transition_run.cpp publishes).
//
// Written write by write, `this.id = id; ...; this.parent = null` in a
// fourteen-field constructor is fourteen misses: each write's static guard
// names the shape AFTER its key is added, each cache way the same, so every
// one falls to the shape-transition arm, and the fifth grows the overflow
// block through the helper. A run asks once whether the receiver is where the
// run started last time, and then is:
//
//   fast   the overflow block if the receiver has none (one bump, sized from
//          the cells), the final shape stored first, then each write a store
//          at its constant slot, the instructions between them as themselves;
//   slow   the stretch exactly as it would have been lowered without this,
//          and then the publish that lets the next receiver take the fast copy.
//
// Installing the final shape before the values is sound because nothing
// between the writes can look: what may stand in a run is decided by
// lower/transition_runs.cpp and re-checked here, and none of it runs user code
// or throws. A collection in between (an empty literal's allocation) finds
// a well-formed object whose later slots are still undefined.

#include "il_abi.h"
#include "il_lowering.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <unordered_set>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int64_t kBarePlainHeader =
    (int64_t{BRONZE_ABI_PLAIN_OBJECT_BYTES} << (8 * BRONZE_ABI_HDR_SIZE_OFFSET)) |
    ((BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT);
constexpr int32_t kCell = static_cast<int32_t>(sizeof(uint64_t));

bool scalar(BronzeType t) {
    return t == BronzeType::F64 || t == BronzeType::I32 || t == BronzeType::Bool;
}

// lower/transition_runs.cpp's `between`, over the backend's instruction: the
// two must agree, and a run this refuses is a diagnosed bug in the marking.
bool between(const BronzeInstruction& inst, const std::unordered_set<uint32_t>& defined) {
    switch (inst.op) {
        case BronzeOp::ConstF64:
        case BronzeOp::ConstI32:
        case BronzeOp::ConstBool:
        case BronzeOp::ConstUndefined:
        case BronzeOp::ConstNull:
        case BronzeOp::IsNumber:
        case BronzeOp::NumTruthy:
        case BronzeOp::CreateObject:
        case BronzeOp::CreateArray:
            return true;
        case BronzeOp::Box:
            return scalar(inst.box_type);
        case BronzeOp::Unbox:
            return inst.result_type == BronzeType::F64 && !inst.operands.empty() &&
                   (inst.raw_unbox || inst.nullish_unbox || !defined.count(inst.operands[0]));
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
            return scalar(inst.result_type);
        default:
            return false;
    }
}

// A ToNumber the guard has proven a Number up front, so the fast copy's is
// the bitcast.
bool checked_unbox(const BronzeInstruction& inst) {
    return inst.op == BronzeOp::Unbox && !inst.raw_unbox && !inst.nullish_unbox;
}

Value* iconst(Builder& b, uint64_t v) {
    return b.build_iconst_i64(static_cast<int64_t>(v));
}

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

} // namespace

bool IlLowering::try_lower_transition_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                          std::unordered_map<uint32_t, Value*>& val_map,
                                          const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                          uint32_t* cont_counter) {
    const auto& insts = blk_ast.instructions;
    const BronzeInstruction& first = insts[inst_idx];
    // An assignment's adds and a literal's or a class's definitions alike: on
    // the chain the publish admits — no setter, read-only property or trap for
    // any key, all of them absent — a [[Set]] and a CreateDataProperty store
    // the same property, so one fast copy serves both.
    const auto is_write = [](BronzeOp op) { return op == BronzeOp::PropSet || op == BronzeOp::PropDefine; };
    if (!is_write(first.op) || first.run_cell == BronzeInstruction::kNoTransitionRun) return false;
    // A block run once calls its helpers (run_once): a guard is only size.
    if (run_once_) return false;
    auto refuse = [&](const std::string& why) {
        if (diag_) diag_->error("il2mir: transition run at cell " + std::to_string(first.run_cell) + " " + why);
        has_error_ = true;
        return true;
    };
    const size_t end = inst_idx + first.run_length;
    if (end > insts.size() || first.run_length < 2 || first.operands.size() != 2) return refuse("overruns its block");
    if (!prop_lowering_.static_cells_cover(first.run_cell, BRONZE_ABI_TRANSITION_RUN_CELLS)) {
        return refuse("is past the module's static cells");
    }
    const uint32_t recv_id = first.operands[0];

    std::vector<size_t> sites;
    std::unordered_set<uint32_t> defined;
    std::vector<uint32_t> proven_numbers;  // checked unboxes' operands
    for (size_t j = inst_idx; j < end; ++j) {
        const BronzeInstruction& inst = insts[j];
        if (is_write(inst.op) && inst.operands.size() == 2 && inst.operands[0] == recv_id) {
            if (defined.count(inst.operands[1]) == 0 && inst.operands[1] == recv_id) return refuse("stores its receiver");
            sites.push_back(j);
        } else if (between(inst, defined)) {
            if (checked_unbox(inst)) proven_numbers.push_back(inst.operands[0]);
        } else {
            return refuse("holds op " + std::to_string(static_cast<int>(inst.op)) + ", which no run may");
        }
        if (inst.result_id == recv_id) return refuse("redefines its receiver");
        if (inst.result_id != UINT32_MAX) defined.insert(inst.result_id);
    }
    if (sites.size() < 2 || sites.back() != end - 1) return refuse("does not end at a write");
    const uint32_t last_slot = first.run_slot + static_cast<uint32_t>(sites.size()) - 1;
    const bool overflow = last_slot >= kBronzeObjInlineSlots;
    const uint64_t need = overflow ? last_slot + 1 - kBronzeObjInlineSlots : 0;

    // Values defined in the run with a use past it: one join parameter each.
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

    const std::string prefix = "trun_" + std::to_string(fn->next_block_id());
    BasicBlock* guard_bb = b.current_block();
    BasicBlock* bb_check = b.append_block(prefix + "_check");
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow_cold");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    BasicBlock* bb_room = overflow ? b.append_block(prefix + "_room") : nullptr;
    BasicBlock* bb_alloc = overflow ? b.append_block(prefix + "_alloc") : nullptr;
    BasicBlock* bb_alloced = overflow ? b.append_block(prefix + "_alloced") : nullptr;
    Value* fast_block = overflow ? b.add_block_param(bb_fast, Type::i64()) : nullptr;

    // The guard, first half: a plain object with no internal slots, at the
    // start shape. Read through receiver_shape's stand-in for a non-object,
    // so the loads are safe before anything is known.
    b.position_at_end(guard_bb);
    apply_inst_loc(b, first);
    Value* obj = ensure_type(get_val_by_id(recv_id, b, val_map), Type::i64(), b);
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    Value* recv_base = nullptr;
    prop_lowering_.receiver_shape(b, obj, plain, ptr, shape, &recv_base);
    Value* cells = b.build_add(prop_lowering_.static_cells_addr(b),
                               iconst(b, uint64_t{first.run_cell} * sizeof(uint64_t)));
    Value* bare = b.build_eq(b.build_load(Type::i64(), recv_base, 0), b.build_iconst_i64(kBarePlainHeader));
    Value* start = b.build_load(Type::i64(), cells, BRONZE_ABI_TRANSITION_RUN_START * kCell);
    // What the publish is told the run started from; 0 for any receiver it
    // must refuse anyway.
    Value* start_seen = b.build_select(bare, shape, iconst(b, 0));
    b.build_br_if(b.build_and(bare, b.build_eq(shape, start)), bb_check, bb_slow);

    // Second half, now that `shape` is the published start: the prototype
    // chain as it was verified, the start no prototype's, each value fit for
    // its slot's representation, and room for the last slot.
    b.position_at_end(bb_check);
    Value* tls = tls_block(b);
    Value* ok = b.build_eq(b.build_load(Type::i64(), cells, BRONZE_ABI_TRANSITION_RUN_EPOCH * kCell),
                           b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    ok = b.build_and(ok, b.build_eq(b.build_zext_i64(b.build_load(Type::i8(), shape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET)),
                                    iconst(b, 0)));
    Value* end_shape = b.build_load(Type::i64(), cells, BRONZE_ABI_TRANSITION_RUN_END * kCell);
    Value* doubles = nullptr;
    for (size_t k = 0; k < sites.size(); ++k) {
        const uint32_t slot = first.run_slot + static_cast<uint32_t>(k);
        const uint32_t val_id = insts[sites[k]].operands[1];
        if (slot >= BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT || is_number_value(val_id)) continue;
        if (!doubles) doubles = b.build_load(Type::i64(), end_shape, BRONZE_ABI_SHAPE_DOUBLESLOTS_OFFSET);
        Value* boxed = b.build_eq(b.build_and(doubles, iconst(b, uint64_t{1} << slot)), iconst(b, 0));
        // A value from before the run can be asked now; one made inside it
        // and not a Number's box (a literal, a constant) never fits a double.
        if (!defined.count(val_id)) {
            Value* bits = ensure_type(get_val_by_id(val_id, b, val_map), Type::i64(), b);
            boxed = b.build_or(boxed, b.build_ule(bits, iconst(b, kBronzeNumberMaxBits)));
        }
        ok = b.build_and(ok, boxed);
    }
    for (uint32_t id : proven_numbers) {
        Value* bits = ensure_type(get_val_by_id(id, b, val_map), Type::i64(), b);
        ok = b.build_and(ok, b.build_ule(bits, iconst(b, kBronzeNumberMaxBits)));
    }
    Value* ov = nullptr;
    Value* has_block = nullptr;
    Value* no_block = nullptr;
    if (overflow) {
        // A block whose capacity, read off its header's size as
        // ObjectHeader::overflowCapacity does, reaches the last slot; or none.
        ov = b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset);
        Value* blk = b.build_xor(ov, iconst(b, kObjectTagBits));
        has_block = b.build_ult(blk, b.build_iconst_i64(kPayloadLimit));
        Value* blk_base = b.build_select(has_block, blk, ptr);
        Value* size = b.build_zext_i64(b.build_load(Type::i32(), blk_base, BRONZE_ABI_HDR_SIZE_OFFSET));
        Value* capacity = b.build_lshr(b.build_sub(size, iconst(b, BRONZE_ABI_HDR_BYTES)), iconst(b, 3));
        Value* fits = b.build_and(has_block, b.build_uge(capacity, iconst(b, need)));
        no_block = b.build_eq(ov, iconst(b, kUndefinedTag));
        ok = b.build_and(ok, b.build_or(fits, no_block));
        b.build_br_if(ok, bb_room, bb_slow);

        b.position_at_end(bb_room);
        b.build_br_if(no_block, bb_alloc, {}, bb_fast, {blk});

        // The capacity the publish chose, never below this run's own need.
        b.position_at_end(bb_alloc);
        Value* cap_addr = b.build_load(Type::i64(), cells, BRONZE_ABI_TRANSITION_RUN_CAPACITY * kCell);
        Value* cap = b.build_zext_i64(b.build_load(Type::i32(), cap_addr, 0));
        BasicBlock* bb_cap = b.current_block();
        BasicBlock* bb_bump = b.append_block(prefix + "_bump");
        b.position_at_end(bb_cap);
        b.build_br_if(b.build_uge(cap, iconst(b, need)), bb_bump, bb_slow);
        b.position_at_end(bb_bump);
        Value* fresh = alloc_lowering_.bump_slot_block(b, cap, bb_slow, bb_alloced);
        // A fresh block holds no reference yet; the receiver may be old, so
        // the store of the block into it takes the barrier, and the call
        // means the receiver is re-read after it.
        Value* ov_val = b.build_or(b.build_and(fresh, iconst(b, kPayloadMask)), iconst(b, kObjectTagBits));
        Value* ov_addr = b.build_add(ptr, iconst(b, kBronzeObjOverflowOffset));
        b.build_store(Type::i64(), ov_addr, 0, ov_val);
        b.build_call("brass_gc_write_barrier", Type::void_type(), {ov_addr, ov_val});
        Value* again = b.build_xor(ensure_type(get_val_by_id(recv_id, b, val_map), Type::i64(), b),
                                   iconst(b, kObjectTagBits));
        Value* moved = b.build_xor(b.build_load(Type::i64(), again, kBronzeObjOverflowOffset), iconst(b, kObjectTagBits));
        b.build_br(bb_fast, {moved});
    } else {
        b.build_br_if(ok, bb_fast, bb_slow);
    }

    // FAST: the final shape, then the run.
    std::unordered_map<uint32_t, Value*> fast_map = val_map;
    b.position_at_end(bb_fast);
    Value* cur_ptr = b.build_xor(ensure_type(get_val_by_id(recv_id, b, fast_map), Type::i64(), b),
                                 iconst(b, kObjectTagBits));
    Value* cur_block = fast_block;
    b.build_store(Type::i64(), cur_ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET, end_shape);
    auto rederive = [&] {
        cur_ptr = b.build_xor(ensure_type(get_val_by_id(recv_id, b, fast_map), Type::i64(), b),
                              iconst(b, kObjectTagBits));
        if (overflow) {
            cur_block = b.build_xor(b.build_load(Type::i64(), cur_ptr, kBronzeObjOverflowOffset),
                                    iconst(b, kObjectTagBits));
        }
    };
    size_t site_k = 0;
    for (size_t j = inst_idx; j < end; ++j) {
        const BronzeInstruction& inst = insts[j];
        apply_inst_loc(b, inst);
        if (site_k < sites.size() && sites[site_k] == j) {
            const uint32_t slot = first.run_slot + static_cast<uint32_t>(site_k++);
            const uint32_t val_id = inst.operands[1];
            Value* val = ensure_type(get_val_by_id(val_id, b, fast_map), Type::i64(), b);
            Value* base = cur_ptr;
            int32_t off = static_cast<int32_t>(kBronzeObjSlotsOffset + slot * 8);
            if (slot >= kBronzeObjInlineSlots) {
                base = cur_block;
                off = static_cast<int32_t>(kBronzeHdrBytes + (slot - kBronzeObjInlineSlots) * 8);
            }
            const bool need_barrier = !is_primitive_value(val_id) && !prop_lowering_.is_known_non_ref(val);
            prop_lowering_.store_with_barrier(b, base, off, val, need_barrier, prefix);
            if (need_barrier) rederive();
            continue;
        }
        BronzeInstruction copy = inst;
        if (checked_unbox(copy)) copy.raw_unbox = true;
        if (!lower_instruction(copy, b, fn, fast_map, block_map, blk_ast.handler_id, blk_ast.id, cont_counter)) {
            return false;
        }
        if (inst.op == BronzeOp::CreateObject || inst.op == BronzeOp::CreateArray) rederive();
    }
    std::vector<Value*> fast_args;
    for (uint32_t id : live_out) fast_args.push_back(fast_map.at(id));
    BasicBlock* fast_end = b.current_block();

    // SLOW: the stretch as it always was, a static run where it holds one,
    // then the publish while the run is still taking them.
    std::unordered_map<uint32_t, Value*> slow_map = val_map;
    b.position_at_end(bb_slow);
    BronzeBlock stretch;
    stretch.id = blk_ast.id;
    stretch.handler_id = blk_ast.handler_id;
    stretch.instructions.assign(insts.begin() + static_cast<std::ptrdiff_t>(inst_idx),
                                insts.begin() + static_cast<std::ptrdiff_t>(end));
    stretch.instructions.front().run_cell = BronzeInstruction::kNoTransitionRun;
    for (size_t j = 0; j < stretch.instructions.size(); ++j) {
        if (try_lower_static_slot_run(stretch, j, b, fn, slow_map, block_map, cont_counter)) {
            if (has_error_) return false;
            continue;
        }
        apply_inst_loc(b, stretch.instructions[j]);
        if (!lower_instruction(stretch.instructions[j], b, fn, slow_map, block_map, blk_ast.handler_id, blk_ast.id,
                               cont_counter)) {
            return false;
        }
    }
    {
        BasicBlock* bb_ran = b.current_block();
        BasicBlock* bb_publish = b.append_block(prefix + "_publish_cold");
        BasicBlock* bb_published = b.append_block(prefix + "_published");
        b.position_at_end(bb_ran);
        Value* count = b.build_lshr(b.build_load(Type::i64(), cells, BRONZE_ABI_TRANSITION_RUN_COUNT * kCell),
                                    iconst(b, BRONZE_ABI_TRANSITION_RUN_COUNT_SHIFT));
        b.build_br_if(b.build_ult(count, iconst(b, BRONZE_ABI_TRANSITION_RUN_PUBLISH_LIMIT)), bb_publish,
                      bb_published);
        b.position_at_end(bb_publish);
        // The keys go in the argv block, as a call's arguments do: small
        // integers, which a root scan reads as Numbers and passes over.
        std::vector<Value*> key_ids;
        for (size_t site : sites) {
            key_ids.push_back(b.build_zext_i64(prop_lowering_.runtime_key(b, insts[site].index)));
        }
        Value* keys = stage_argv(b, key_ids);
        Value* recv_now = ensure_type(get_val_by_id(recv_id, b, slow_map), Type::i64(), b);
        Value* cells_now = b.build_add(prop_lowering_.static_cells_addr(b),
                                       iconst(b, uint64_t{first.run_cell} * sizeof(uint64_t)));
        b.build_call("bronze_transition_run_publish", Type::void_type(),
                     {recv_now, start_seen, cells_now, keys, b.build_iconst_i32(static_cast<int32_t>(sites.size())),
                      b.build_iconst_i32(static_cast<int32_t>(first.run_slot))});
        b.build_br(bb_published);
        bb_publish->set_cold();
        b.position_at_end(bb_published);
    }
    std::vector<Value*> slow_args;
    for (size_t k = 0; k < live_out.size(); ++k) {
        slow_args.push_back(ensure_type(slow_map.at(live_out[k]), fast_args[k]->type(), b));
    }
    BasicBlock* slow_end = b.current_block();

    for (size_t k = 0; k < live_out.size(); ++k) {
        val_map[live_out[k]] = b.add_block_param(bb_merge, fast_args[k]->type());
    }
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
