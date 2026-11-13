// The inline Array arms (il_array_method.h). Each is the runtime native's own
// fast path, test for test (builtin_array_mutators.cpp), so the arm and the
// native it skips can never disagree about which arrays they answer for:
//
//   push   an Array with no named-property box and room in its block for all
//          k = 1..8 pushed elements (head + length + k - 1 < capacity), while
//          the TLS array index hazard is clear: the values stored there,
//          `length` k more, and the new length returned.
//   shift  an Array with no box and at least one element, while the TLS array
//          index hazard says no chain holds an index: the first
//          element (a hole reads as undefined) taken out, a hole left in its
//          place, the head moved past it, `length` one less, and both reset to
//          the block's start when the array empties.
//
// The box test is what answers for every array these must not touch. A frozen,
// sealed or non-extensible array has one (its integrity level lives on it),
// and so does an array with an own named property, with a custom prototype
// (whose elements a hole would read through), or a subclass instance; each
// takes the site's call, which reaches the native's full algorithm. No
// element-level attribute exists without a box, which is also why the stored
// slot needs no writability test.

#include "il_array_method.h"

#include "il_abi.h"
#include "il_array_sort.h"
#include "il_lowering.h"
#include "il_lowering_direct.h"
#include "il_property.h"
#include "il/il.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

Value* tls_block(Builder& b) {
    Module* mod = b.current_block()->parent()->parent();
    return (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                               : b.build_call("bronze_tls_block_addr", Type::i64(), {});
}

// A count as a Number's bits: an exact integer below 2^32, so no NaN to
// canonicalize.
Value* box_count(Builder& b, Value* n) { return b.build_bitcast_i64_f64(b.build_sitofp_f64_i64(n)); }

}  // namespace

ArrayInline array_method_inline(IlLowering* lowering, const BronzeInstruction& inst) {
    if (!lowering || !direct_method_ic_here(lowering) || lowering->run_once()) return ArrayInline::None;
    PropertyLoweringHelper& pl = lowering->prop_lowering();
    if (!pl.enable_inlined_fastpaths()) return ArrayInline::None;
    const std::string key(pl.key_tag(inst.index));
    if (key == "sort" && inst.param_count == 1) return ArrayInline::Sort;
    if ((inst.imm_i64 & bronze::il::Instruction::kMethodRecvArray) == 0) return ArrayInline::None;
    if (key == "push" && inst.param_count >= 1 && inst.param_count <= 8) return ArrayInline::Push;
    if (key == "shift" && inst.param_count == 0) return ArrayInline::Shift;
    return ArrayInline::None;
}

Value* lower_array_method_inline(IlLowering* lowering, Builder& b, ArrayInline which, Value* recv,
                                 bool recv_is_array, const std::vector<ArrayPushArg>& args,
                                 const std::function<Value*(Builder&)>& slow) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = std::string(which == ArrayInline::Push    ? "arr_push_"
                                           : which == ArrayInline::Shift ? "arr_shift_"
                                                                         : "arr_sort_") +
                               std::to_string(fn->next_block_id());
    BasicBlock* bb_arr = b.append_block(prefix + "_array");
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_call");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());
    bb_slow->set_cold();

    // An Array: the header is read through the TLS block (mapped, and its
    // first word no header: bronze_abi.h, proto_epoch) when the value is no
    // object at all, so the header compare alone refuses it.
    // A receiver the function made as an Array (is_array_value) is one.
    b.position_at_end(cur);
    Value* ptr = b.build_xor(recv, iconst(b, kObjectTagBits));
    if (recv_is_array) {
        b.build_br(bb_arr);
    } else {
        Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
        Value* base = b.build_select(is_obj, ptr, tls_block(b));
        Value* is_arr = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kArrayHeaderLow));
        b.build_br_if(is_arr, bb_arr, bb_slow);
    }

    b.position_at_end(bb_arr);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* props = b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    // The word is `undefined` until the array is given its box (array.cpp).
    Value* no_props = b.build_eq(props, iconst(b, kUndefinedTag));
    if (which == ArrayInline::Sort) {
        // An Array with no properties object finds the intrinsic `sort` (the
        // header says why), and the runtime does everything the builtin does
        // around the merge, holes and the chain's indices included.
        b.build_br_if(no_props, bb_fast, bb_slow);
        b.position_at_end(bb_fast);
        b.build_br(bb_merge, {b.build_call("bronze_array_sort_merged", Type::i64(),
                                           {recv, args[0].bits, b.build_func_addr(kSortMergeStub)})});
        b.position_at_end(bb_slow);
        const size_t slow_base = fn->blocks().size();
        b.build_br(bb_merge, {slow(b)});
        for (size_t bi = slow_base; bi < fn->blocks().size(); ++bi) fn->blocks()[bi]->set_cold();
        b.position_at_end(bb_merge);
        return result;
    }
    Value* pos = nullptr;
    const size_t k = args.size();
    // And the array index hazard (bronze_abi_tls.h, array_index_hazard): a
    // push writes at `length .. length + k - 1`, which a setter on the chain
    // would take, and a shift moves the block down as it stands, which is the
    // specification's walk only while no hole could read through to the chain.
    // Both natives ask the same (rtArrayChainHoldsIndex); a miss reaches them.
    // The word folds into a bound each tests anyway (bronze_abi_tls.h): the
    // last written slot OR'd with it is below no capacity, and it is below no
    // length.
    Value* hazard = lowering->prop_lowering().index_hazard_word(b);
    if (which == ArrayInline::Push) {
        Value* capacity = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
        pos = b.build_add(head, length);
        Value* last_pos = k == 1 ? pos : b.build_add(pos, iconst(b, k - 1));
        b.build_br_if(b.build_and(no_props, b.build_ult(b.build_or(last_pos, hazard), capacity)), bb_fast, bb_slow);
    } else {
        b.build_br_if(b.build_and(no_props, b.build_ult(hazard, length)), bb_fast, bb_slow);
    }

    b.position_at_end(bb_fast);
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    if (which == ArrayInline::Push) {
        // Every slot past `length` up to `capacity` is hole-filled, so all k
        // stores append into holes and nothing is overwritten.
        for (size_t j = 0; j < k; ++j) {
            const auto disp = static_cast<int32_t>(kBronzeHdrBytes + j * 8);
            with_alias_class(b.build_store_indexed(Type::i64(), block, pos, 8, disp, args[j].bits),
                             kBoxedElementAliasClass);
        }
        Value* new_len = b.build_add(length, iconst(b, k));
        b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, b.build_trunc_i32(new_len));
        Value* boxed_len = box_count(b, new_len);
        std::vector<size_t> unproven;
        for (size_t j = 0; j < k; ++j) {
            if (!args[j].proven_number && !lowering->prop_lowering().is_known_number(args[j].bits)) {
                unproven.push_back(j);
            }
        }
        if (unproven.empty()) {
            b.build_br(bb_merge, {boxed_len});
        } else {
            // The block is a heap object like any other, so a reference stored
            // in it takes the barrier every element store does (every reference
            // is tagged above the Number range).
            BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
            BasicBlock* bb_stored = b.append_block(prefix + "_stored");
            b.position_at_end(bb_fast);
            Value* max_num = iconst(b, kBronzeNumberMaxBits);
            std::vector<Value*> non_nums;
            non_nums.reserve(unproven.size());
            Value* any_non_num = nullptr;
            for (size_t j : unproven) {
                Value* non_num = b.build_ugt(args[j].bits, max_num);
                non_nums.push_back(non_num);
                any_non_num = any_non_num ? b.build_or(any_non_num, non_num) : non_num;
            }
            b.build_br_if(any_non_num, bb_barrier, bb_stored);
            b.position_at_end(bb_barrier);
            lowering->prop_lowering().forget_numbers(b, recv);  // il_number_array.cpp, the rule for a store
            Value* base_slot = b.build_add(block, b.build_shl(pos, iconst(b, 3)));
            if (unproven.size() == 1) {
                const size_t j = unproven[0];
                Value* slot_addr = b.build_add(base_slot, iconst(b, kBronzeHdrBytes + j * 8));
                b.build_write_barrier(slot_addr, args[j].bits);
                b.build_br(bb_stored);
            } else {
                for (size_t t = 0; t < unproven.size(); ++t) {
                    const size_t j = unproven[t];
                    BasicBlock* bb_cur = b.current_block();
                    BasicBlock* bb_wb = b.append_block(prefix + "_wb_" + std::to_string(j));
                    BasicBlock* bb_next =
                        t + 1 < unproven.size() ? b.append_block(prefix + "_wbn_" + std::to_string(j)) : bb_stored;
                    b.position_at_end(bb_cur);
                    b.build_br_if(non_nums[t], bb_wb, bb_next);
                    b.position_at_end(bb_wb);
                    Value* slot_addr = b.build_add(base_slot, iconst(b, kBronzeHdrBytes + j * 8));
                    b.build_write_barrier(slot_addr, args[j].bits);
                    b.build_br(bb_next);
                    b.position_at_end(bb_next);
                }
            }
            b.position_at_end(bb_stored);
            b.build_br(bb_merge, {boxed_len});
        }
    } else {
        Value* first = b.build_load_indexed(Type::i64(), block, head, 8, static_cast<int32_t>(kBronzeHdrBytes));
        // A hole stores no reference, so it needs no barrier.
        b.build_store_indexed(Type::i64(), block, head, 8, static_cast<int32_t>(kBronzeHdrBytes),
                              iconst(b, BRONZE_ABI_HOLE_BITS));
        Value* new_len = b.build_sub(length, iconst(b, 1));
        Value* empty = b.build_eq(new_len, iconst(b, 0));
        Value* new_head = b.build_select(empty, iconst(b, 0), b.build_add(head, iconst(b, 1)));
        b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, b.build_trunc_i32(new_len));
        b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET, b.build_trunc_i32(new_head));
        Value* is_hole = b.build_eq(first, iconst(b, BRONZE_ABI_HOLE_BITS));
        b.build_br(bb_merge, {b.build_select(is_hole, iconst(b, kUndefinedTag), first)});
    }

    b.position_at_end(bb_slow);
    const size_t slow_base = fn->blocks().size();
    b.build_br(bb_merge, {slow(b)});
    for (size_t bi = slow_base; bi < fn->blocks().size(); ++bi) fn->blocks()[bi]->set_cold();

    b.position_at_end(bb_merge);
    return result;
}

}  // namespace il2mir
