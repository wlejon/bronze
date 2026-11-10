// Keyed element reads and writes on a receiver PROVEN an Array: `is.array`
// (il_op.h) dominates them, and the typed-loop versioning pass marked them
// with kElemFlagArrayReceiver (il_pin.h). The heap kind never changes, so
// what a site skips is exactly the generic path's first two questions — is
// it an object, is its header an Array's — and nothing that changes with the
// array: the index test (unless kElemFlagProvenIntegralIndex), the bounds, the
// capacity and props word of a store, the hole rule and the write barrier are
// the generic Array arm's (il_elem_lowering.cpp), and every miss is its helper.

#include "il/il_pin.h"
#include "il_abi.h"
#include "il_elem_stub.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include "il_write_barrier.h"
#include <brass/mir/function.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

}  // namespace

bool PropertyLoweringHelper::elem_array_receiver(int64_t flags) {
    return (flags & bronze::il::kElemFlagArrayReceiver) != 0;
}

// Branches, as is.typed_array does: the test is made once per entry of the
// loop it versions.
Value* PropertyLoweringHelper::lower_is_array(Builder& b, Value* obj) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "is_arr_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    b.position_at_end(cur);
    Value* is_obj = b.build_eq(b.build_and(obj, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    b.build_br_if(is_obj, bb_obj, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_obj);
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    b.build_br(bb_merge, {b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow))});

    b.position_at_end(bb_merge);
    return result;
}

Value* PropertyLoweringHelper::lower_elem_get_array(Builder& b, Value* obj, Value* index, int64_t flags) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_get_a_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_array = b.append_block(prefix + "_array");
    BasicBlock* bb_load = b.append_block(prefix + "_load");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(cur);
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    Value* idx = nullptr;
    Value* ok = nullptr;
    emit_elem_index_guard(b, index, idx, ok, (flags & bronze::il::kElemFlagProvenIntegralIndex) != 0);
    b.build_br_if(ok, bb_array, bb_slow);

    b.position_at_end(bb_array);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    b.build_br_if(b.build_ult(idx, length), bb_load, bb_slow);

    b.position_at_end(bb_load);
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* elem = b.build_load_indexed(Type::i64(), block, b.build_add(idx, head), 8,
                                       static_cast<int32_t>(kBronzeHdrBytes));
    finish_hole_read(b, elem, bb_merge, bb_slow, prefix);

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {b.build_call(elem_get_helper(), Type::i64(), {obj, boxed_elem_key(b, index)})});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::lower_elem_set_array(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict,
                                                  int64_t flags) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_set_a_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_array = b.append_block(prefix + "_array");
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(cur);
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    Value* idx = nullptr;
    Value* ok = nullptr;
    emit_elem_index_guard(b, index, idx, ok, (flags & bronze::il::kElemFlagProvenIntegralIndex) != 0);
    b.build_br_if(ok, bb_array, bb_slow);

    // An element in bounds or the append at `length`, and no properties
    // object (il_number_array.cpp, branch_on_array_store).
    b.position_at_end(bb_array);
    Value* block = nullptr;
    Value* slot = nullptr;
    branch_on_array_store(b, ptr, idx, bb_store, bb_slow, prefix, block, slot);

    b.position_at_end(bb_store);
    with_alias_class(b.build_store_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes), val),
                     kBoxedElementAliasClass);
    b.build_br_if(b.build_ugt(val, iconst(b, kBronzeNumberMaxBits)), bb_barrier, bb_merge);

    b.position_at_end(bb_barrier);
    forget_numbers(b, obj);
    Value* slot_addr = b.build_add(b.build_add(block, b.build_shl(slot, iconst(b, 3))), iconst(b, kBronzeHdrBytes));
    build_write_barrier(b, slot_addr, val);
    b.build_br(bb_merge);

    b.position_at_end(bb_slow);
    b.build_call(elem_set_helper(), Type::void_type(),
                 {obj, boxed_elem_key(b, index), val, b.build_iconst_i32(static_cast<int32_t>(strict))});
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

}  // namespace il2mir
