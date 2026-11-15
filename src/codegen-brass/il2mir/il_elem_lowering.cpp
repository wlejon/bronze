// Keyed element reads and writes (`o[i]`): the runtime helpers
// (bronze_elem_get / bronze_elem_set), and the receiver tests the IL names
// (`is.dense_array`, `is.typed_array`, `is.plain_array`).
//
// The layouts are the runtime's (runtime/array.h, runtime/typed_array.h),
// pinned by the BRONZE_ABI_* offsets their static_asserts check.

#include "il/il_op.h"
#include "il_abi.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
// The header word's low half: the object tag, then HeapObjectHeader::flags.
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kTypedArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

// The payload pointer of `obj` and whether it is an object at all.
void object_pointer(Builder& b, Value* obj, Value*& ptr, Value*& is_obj) {
    is_obj = is_object_address(b, object_address(b, obj));
    ptr = object_address(b, obj);
}

// The TLS block's word at `off`.
Value* tls_block_word(Builder& b, int32_t off) {
    Module* mod = b.current_block()->parent()->parent();
    Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    return b.build_load(Type::i64(), tls, off);
}

}  // namespace

// An element key as the Value the helpers take: an i32 is an int32 Number, an
// f64 its boxed bits, anything else already a Value.
Value* boxed_elem_key(Builder& b, Value* index) {
    if (index->type() == Type::i32()) return b.build_bitcast_i64_f64(b.build_sitofp_f64_i32(index));
    if (index->type() == Type::f64()) return box_f64_bits(b, index);
    return index;
}

Value* PropertyLoweringHelper::lower_elem_get(Builder& b, Value* obj, Value* index) {
    return b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_elem_key(b, index)});
}

void PropertyLoweringHelper::lower_elem_set(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict) {
    b.build_call("bronze_elem_set", Type::void_type(),
                 {obj, boxed_elem_key(b, index), val, b.build_iconst_i32(static_cast<int32_t>(strict))});
}

bool PropertyLoweringHelper::is_known_non_ref(Value* val) const {
    auto check = [&](auto& self, Value* v, int depth) -> bool {
        if (!v || depth > 4) return false;
        if (boxed_f64_source(v) != nullptr) return true;
        const Instruction* def = v->defining_instruction();
        while (def && (def->opcode() == Opcode::bitcast_i64_tagged || def->opcode() == Opcode::bitcast_tagged_i64) &&
               def->operand_count() >= 1) {
            v = def->operand(0);
            if (boxed_f64_source(v) != nullptr) return true;
            def = v->defining_instruction();
        }
        if (!def) return false;
        if (def->opcode() == Opcode::iconst_i64) {
            const uint64_t imm = static_cast<uint64_t>(def->imm_i64());
            return imm <= kBronzeNumberMaxBits || imm == kUndefinedTag || imm == kNullTag ||
                   imm == kBoolTag || imm == (kBoolTag | 1) || (imm & kTagMask) == kInt32Tag;
        }
        if (def->opcode() == Opcode::bitcast_i64_f64) return true;
        if (def->opcode() == Opcode::select && def->operand_count() >= 3) {
            return self(self, def->operand(1), depth + 1) && self(self, def->operand(2), depth + 1);
        }
        if (def->opcode() == Opcode::or_ && def->operand_count() == 2) {
            for (int side = 0; side < 2; ++side) {
                const Instruction* tag_def = def->operand(side)->defining_instruction();
                if (!tag_def || tag_def->opcode() != Opcode::iconst_i64) continue;
                const uint64_t tag = static_cast<uint64_t>(tag_def->imm_i64());
                if (tag != kInt32Tag && tag != kBoolTag) continue;
                const Instruction* pay_def = def->operand(1 - side)->defining_instruction();
                if (!pay_def) continue;
                if (pay_def->opcode() == Opcode::zext_i64) return true;
                if (pay_def->opcode() == Opcode::and_ && pay_def->operand_count() == 2) {
                    const Instruction* mask_def = pay_def->operand(1)->defining_instruction();
                    if (mask_def && mask_def->opcode() == Opcode::iconst_i64 &&
                        static_cast<uint64_t>(mask_def->imm_i64()) <= 0xFFFFFFFFULL) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    return check(check, val, 0);
}

// `is.dense_array`: an Array with an elements block, `max_index < length`, and
// the array index hazard clear (the TLS tag mask the elements word is ANDed
// with zeroes once it is set), as an i32.
Value* PropertyLoweringHelper::lower_is_dense_array(Builder& b, Value* obj, uint32_t max_index) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "is_dense_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    b.build_br_if(is_obj, bb_obj, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_obj);
    Value* is_arr = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow));
    b.build_br_if(is_arr, bb_arr, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_arr);
    Value* elems = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    Value* mask = tls_block_word(b, BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF);
    Value* has_elems = b.build_eq(b.build_and(elems, mask), iconst(b, kObjectTagBits));
    Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    Value* in_bounds = b.build_ult(b.build_iconst_i32(static_cast<int32_t>(max_index)), len);
    b.build_br(bb_merge, {b.build_and(has_elems, in_bounds)});

    b.position_at_end(bb_merge);
    return result;
}

// Whether the view at `ptr`, of element kind `kind` (an i64), answers
// `length` from its header the way the runtime's read would
// (rtTypedArrayChainPristine): its kind's intrinsic instance shape, and the
// family verified pristine at the current prototype-mutation epoch (TLS
// ta_length_tbl). An i32.
Value* PropertyLoweringHelper::typed_array_chain_pristine(Builder& b, Value* ptr, Value* kind) {
    Module* mod = b.current_block()->parent()->parent();
    Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    Value* tbl = b.build_load(Type::i64(), tls, BRONZE_TLS_TA_LENGTH_TBL_OFF);
    Value* shape_addr = b.build_add(tbl, b.build_shl(kind, b.build_iconst_i64(3)));
    Value* shape_ok = b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                                 b.build_load(Type::i64(), shape_addr, BRONZE_ABI_TA_LENGTH_TBL_SHAPES_OFFSET));
    Value* epoch_ok = b.build_eq(b.build_load(Type::i64(), tbl, BRONZE_ABI_TA_LENGTH_TBL_EPOCH_OFFSET),
                                 b.build_load(Type::i64(), tls, BRONZE_TLS_PROTO_EPOCH_OFF));
    return b.build_and(shape_ok, epoch_ok);
}

Value* PropertyLoweringHelper::lower_is_typed_array(Builder& b, Value* obj, int64_t imm) {
    const int64_t elem_kind = imm & 0xFF;
    const bool pristine = (imm & bronze::il::kTypedArrayPristineLength) != 0;
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "is_ta_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_view = b.append_block(prefix + "_view");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    b.build_br_if(is_obj, bb_obj, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_obj);
    Value* is_view = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kTypedArrayHeaderLow));
    b.build_br_if(is_view, bb_view, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_view);
    Value* kind = b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET);
    Value* kind_ok = b.build_eq(kind, b.build_iconst_i32(static_cast<int32_t>(elem_kind)));
    if (pristine) {
        kind_ok = b.build_and(kind_ok, typed_array_chain_pristine(b, ptr, iconst(b, static_cast<uint64_t>(elem_kind))));
    }
    b.build_br(bb_merge, {kind_ok});

    b.position_at_end(bb_merge);
    return result;
}

// `is.plain_array` (il_op.h): an Array whose props word holds no object.
Value* PropertyLoweringHelper::lower_is_plain_array(Builder& b, Value* obj) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "is_parr_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i32());

    b.position_at_end(cur);
    Value* is_obj = is_object_address(b, object_address(b, obj));
    b.build_br_if(is_obj, bb_obj, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_obj);
    Value* ptr = object_address(b, obj);
    Value* is_arr = b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow));
    b.build_br_if(is_arr, bb_arr, {}, bb_merge, {b.build_iconst_i32(0)});

    b.position_at_end(bb_arr);
    Value* props = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    b.build_br(bb_merge, {b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits))});

    b.position_at_end(bb_merge);
    return result;
}

Value* PropertyLoweringHelper::box_number(Builder& b, Value* f64_val) {
    return box_f64_bits(b, f64_val);
}

} // namespace il2mir
