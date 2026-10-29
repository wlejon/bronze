// The NUMBERS bit of an Array (runtime/array.h) as generated code meets it:
// the claim `is.number_array` makes in front of a kernel's constant-index
// reads, and the one rule every inline element store owes it.
//
// THE RULE FOR A STORE. The bit promises that every element below `length`
// is a Number in double form. An inline store lands in bounds (or appends at
// `length`), so it can falsify the bit only by storing something that is not
// such a Number — and every such value is tagged above
// BRONZE_ABI_NUMBER_MAX_BITS, the same test that sends a reference to the
// write barrier. So a store clears the bit on that branch: free on a Number,
// one `and` on the header beside a barrier call otherwise. A store whose
// barrier the lowering proved away (a primitive that is not a reference, such
// as a `bool`) clears it by a select on the same test, unless the value is
// known to be a Number.
//
// Nothing generated SETS the bit except the claim's helper, which scans.

#include "il_abi.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;

}  // namespace

// Declared in il_lowering_ops.h. The f64 an i64 Value was boxed from here (box_f64_bits, IlLowering::ensure_type:
// a select of the canonical NaN and the bits), or null.
Value* boxed_f64_source(Value* v) {
    const Instruction* def = v ? v->defining_instruction() : nullptr;
    if (def && def->opcode() == Opcode::bitcast_i64_tagged && def->operand_count() >= 1) {
        const Instruction* inner = def->operand(0)->defining_instruction();
        if (inner && inner->opcode() == Opcode::bitcast_tagged_i64 && inner->operand_count() >= 1) {
            def = inner->operand(0)->defining_instruction();
        }
    }
    if (!def || def->opcode() != Opcode::select || def->operand_count() < 3) return nullptr;
    const Instruction* nan = def->operand(1)->defining_instruction();
    if (!nan || nan->opcode() != Opcode::iconst_i64 ||
        static_cast<uint64_t>(nan->imm_i64()) != BRONZE_ABI_CANONICAL_NAN_BITS) {
        return nullptr;
    }
    const Instruction* bits = def->operand(2)->defining_instruction();
    if (!bits || bits->opcode() != Opcode::bitcast_i64_f64 || bits->operand(0)->type() != Type::f64()) return nullptr;
    return bits->operand(0);
}

Value* PropertyLoweringHelper::lower_is_number_array(Builder& b, Value* obj, uint32_t max_index,
                                                     bool proven_dense) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "is_num_arr_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = proven_dense ? nullptr : b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    // Named cold so block layout moves them out of the kernel: they run when
    // the bit is clear, which a numeric array leaves after its first claim.
    BasicBlock* bb_claim = b.append_block(prefix + "_claim_cold");
    BasicBlock* bb_call = b.append_block(prefix + "_call_cold");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* in_bounds = nullptr;
    Value* len = nullptr;
    if (proven_dense) {
        // An Array with `max_index` below its length, proven on this path: only
        // the bit is left to read.
        ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
        b.build_br(bb_arr);
        b.position_at_end(bb_arr);
        len = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    } else {
        Value* is_obj = nullptr;
        object_payload(b, obj, ptr, is_obj);
        b.build_br_if(is_obj, bb_obj, {}, bb_merge, {b.build_iconst_i32(0)});

        b.position_at_end(bb_obj);
        Value* is_arr = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow));
        b.build_br_if(is_arr, bb_arr, {}, bb_merge, {b.build_iconst_i32(0)});

        b.position_at_end(bb_arr);
        len = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        in_bounds = b.build_ult(b.build_iconst_i32(static_cast<int32_t>(max_index)), len);
    }
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    Value* has_bit = b.build_ne(b.build_and(bits, b.build_iconst_i32(BRONZE_ABI_ARRAY_BIT_NUMBERS)),
                                b.build_iconst_i32(0));
    b.build_br_if(proven_dense ? has_bit : b.build_and(in_bounds, has_bit), bb_merge, {b.build_iconst_i32(1)},
                  bb_claim, {});

    // The bit is clear: an array short enough to scan gets one, and its answer
    // is the claim's. One too long, or too short for the reads, is answered
    // no without a call.
    b.position_at_end(bb_claim);
    Value* scannable = b.build_ule(len, b.build_iconst_i32(BRONZE_ABI_ARRAY_CLAIM_MAX_LENGTH));
    b.build_br_if(proven_dense ? scannable : b.build_and(in_bounds, scannable), bb_call, {}, bb_merge,
                  {b.build_iconst_i32(0)});

    b.position_at_end(bb_call);
    Value* claimed = b.build_call("bronze_array_claim_numbers", Type::i32(), {obj});
    b.build_br(bb_merge, {claimed});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::forget_numbers(Builder& b, Value* obj) {
    Value* ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                  b.build_and(bits, b.build_iconst_i32(~static_cast<int32_t>(BRONZE_ABI_ARRAY_BIT_NUMBERS))));
}

void PropertyLoweringHelper::forget_numbers_unless_number(Builder& b, Value* obj, Value* val) {
    Value* ptr = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    Value* not_num = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    Value* mask = b.build_select(not_num, b.build_iconst_i32(~static_cast<int32_t>(BRONZE_ABI_ARRAY_BIT_NUMBERS)),
                                 b.build_iconst_i32(-1));
    Value* bits = b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
    b.build_store(Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET, b.build_and(bits, mask));
}

bool PropertyLoweringHelper::is_known_number(Value* val) const {
    if (!val) return false;
    if (val->type() == Type::f64()) return true;
    if (boxed_f64_source(val) != nullptr) return true;
    const Instruction* def = val->defining_instruction();
    if (def && def->opcode() == Opcode::iconst_i64) {
        return static_cast<uint64_t>(def->imm_i64()) <= kBronzeNumberMaxBits;
    }
    return false;
}

}  // namespace il2mir
