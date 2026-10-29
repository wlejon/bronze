// Keyed element reads and writes (`o[i]`): the inline paths for an Array or a
// TypedArray read or written at an integral in-bounds index, and the runtime
// helpers (bronze_elem_get / bronze_elem_set) for everything else.
//
// The layouts are the runtime's (runtime/array.h, runtime/typed_array.h),
// pinned by the BRONZE_ABI_* offsets their static_asserts check.

#include "il/il_op.h"
#include "il_abi.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <cmath>
#include <cstdlib>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kPayloadLimit = uint64_t{1} << 48;
// The header word's low half: the object tag, then HeapObjectHeader::flags.
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kTypedArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

// A non-object's header and array words, for an in-process module: the zeroed
// block's header is no Array's, so branchless header/length/elements loads
// through it safely refuse non-objects.
alignas(16) const uint64_t kNotAnArray[8] = {};

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

// An element index: `i64` is the index when `ok` (an integral Number; the
// element paths range-check it against an unsigned length, so a negative one
// misses there). The boxed Value for the slow-path helper is materialized only
// in `bb_slow`.
struct ElemIndex {
    Value* i64 = nullptr;
    Value* ok = nullptr;
};

// The f64 an i64 Value was boxed from here (box_f64_bits, IlLowering::ensure_type:
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

// The index as the Value the helpers take: an i32 is an int32 Number.
Value* boxed_index(Builder& b, Value* index) {
    if (index->type() == Type::i32()) return b.build_bitcast_i64_f64(b.build_sitofp_f64_i32(index));
    if (index->type() == Type::f64()) return box_f64_bits(b, index);
    return index;
}

ElemIndex elem_index(Builder& b, Value* index, bool proven_integral = false) {
    ElemIndex r;
    if (index->type() == Type::i32()) {
        r.i64 = b.build_sext_i64(index);
        r.ok = b.build_iconst_i32(1);
        return r;
    }
    Value* f = nullptr;
    Value* is_number = nullptr;
    if (index->type() == Type::f64()) {
        f = index;
    } else if (Value* src = boxed_f64_source(index)) {
        f = src;
    } else {
        // A Number's bits are the double; any other Value is a tag above them.
        is_number = b.build_ule(index, iconst(b, kBronzeNumberMaxBits));
        f = b.build_bitcast_f64_i64(index);
    }
    if (f && f->is_instruction()) {
        const Instruction* fdef = f->defining_instruction();
        if (fdef) {
            if (fdef->opcode() == Opcode::sitofp_f64_i32 && fdef->operand_count() == 1) {
                r.i64 = b.build_sext_i64(fdef->operand(0));
                r.ok = is_number ? is_number : b.build_iconst_i32(1);
                return r;
            }
            if (fdef->opcode() == Opcode::sitofp_f64_i64 && fdef->operand_count() == 1) {
                Value* z = fdef->operand(0);
                if (z && z->is_instruction()) {
                    const Instruction* zdef = z->defining_instruction();
                    if (zdef && zdef->opcode() == Opcode::zext_i64 && zdef->operand_count() == 1 &&
                        zdef->operand(0)->type() == Type::i32()) {
                        r.i64 = z;
                        r.ok = is_number ? is_number : b.build_iconst_i32(1);
                        return r;
                    }
                }
            }
            if (fdef->opcode() == Opcode::fconst_f64 && std::isfinite(fdef->imm_f64()) &&
                std::floor(fdef->imm_f64()) == fdef->imm_f64() &&
                std::fabs(fdef->imm_f64()) <= 2147483647.0) {
                r.i64 = iconst(b, static_cast<uint64_t>(static_cast<int64_t>(fdef->imm_f64())));
                r.ok = is_number ? is_number : b.build_iconst_i32(1);
                return r;
            }
        }
    }
    // Integral when the round trip is exact (the compare is ordered: NaN
    // fails it; -0 reads as index 0, as ToPropertyKey makes it "0").
    r.i64 = b.build_fptosi_i64(f);
    if (proven_integral) {
        r.ok = is_number ? is_number : b.build_iconst_i32(1);
        return r;
    }
    Value* exact = b.build_eq(b.build_sitofp_f64_i64(r.i64), f);
    r.ok = is_number ? b.build_and(is_number, exact) : exact;
    return r;
}

// The payload pointer of `obj` and whether it is an object at all.
void object_pointer(Builder& b, Value* obj, Value*& ptr, Value*& is_obj) {
    is_obj = b.build_eq(b.build_and(obj, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    ptr = b.build_and(obj, iconst(b, kPayloadMask));
}

// The first byte of the typed array at `ptr`'s window: its buffer's external
// store, or the bytes inline after the buffer's header, plus byteOffset.
Value* typed_array_data(Builder& b, Value* ptr) {
    Value* buf = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_TA_BUFFER_OFFSET), iconst(b, kPayloadMask));
    Value* ext = b.build_load(Type::i64(), buf, BRONZE_ABI_BUF_EXTPTR_OFFSET);
    Value* inline_data = b.build_add(buf, iconst(b, BRONZE_ABI_BUF_DATA_OFFSET));
    Value* data = b.build_select(b.build_ne(ext, iconst(b, 0)), ext, inline_data);
    Value* byte_offset = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET));
    return b.build_add(data, byte_offset);
}

// Element `idx` of a 16-bit array at `data`, zero-extended to i64. MIR widens
// an i8 but not an i16, so it is read as its two bytes (little-endian, as
// every target brass runs typed arrays on is).
Value* load_u16(Builder& b, Value* data, Value* idx) {
    Value* lo = b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx, 2, 0));
    Value* hi = b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx, 2, 1));
    return b.build_or(lo, b.build_shl(hi, iconst(b, 8)));
}

// The low `bits` of `v` (an i64 holding them zero-extended) sign-extended.
Value* sign_extend(Builder& b, Value* v, int bits) {
    Value* shift = iconst(b, static_cast<uint64_t>(64 - bits));
    return b.build_ashr(b.build_shl(v, shift), shift);
}

Value* load_typed_array_elem(Builder& b, Value* data, Value* idx_i64, int64_t elem_kind) {
    switch (elem_kind) {
        case BRONZE_ABI_TA_KIND_FLOAT64:
            return b.build_load_indexed(Type::f64(), data, idx_i64, 8);
        case BRONZE_ABI_TA_KIND_FLOAT32:
            return b.build_fpext_f64_f32(b.build_load_indexed(Type::f32(), data, idx_i64, 4));
        case BRONZE_ABI_TA_KIND_INT32:
            return b.build_sitofp_f64_i32(b.build_load_indexed(Type::i32(), data, idx_i64, 4));
        case BRONZE_ABI_TA_KIND_UINT32:
            return b.build_sitofp_f64_i64(b.build_zext_i64(b.build_load_indexed(Type::i32(), data, idx_i64, 4)));
        case BRONZE_ABI_TA_KIND_INT16:
            return b.build_sitofp_f64_i64(sign_extend(b, load_u16(b, data, idx_i64), 16));
        case BRONZE_ABI_TA_KIND_UINT16:
            return b.build_sitofp_f64_i64(load_u16(b, data, idx_i64));
        case BRONZE_ABI_TA_KIND_INT8:
            return b.build_sitofp_f64_i64(
                sign_extend(b, b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx_i64, 1)), 8));
        default: // UINT8, UINT8CLAMPED
            return b.build_sitofp_f64_i64(b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx_i64, 1)));
    }
}

Value* extract_i32_from_sitofp(Value* v) {
    if (!v || !v->is_instruction()) return nullptr;
    const Instruction* def = v->defining_instruction();
    if (!def) return nullptr;
    if (def->opcode() == Opcode::sitofp_f64_i32 && def->operand_count() == 1) {
        return def->operand(0);
    }
    if (def->opcode() == Opcode::sitofp_f64_i64 && def->operand_count() == 1) {
        Value* z = def->operand(0);
        if (z && z->is_instruction()) {
            const Instruction* zdef = z->defining_instruction();
            if (zdef && zdef->opcode() == Opcode::zext_i64 && zdef->operand_count() == 1 &&
                zdef->operand(0)->type() == Type::i32()) {
                return zdef->operand(0);
            }
        }
    }
    return nullptr;
}

Value* narrow_f64_to_i32_callfree(Builder& b, Value* v) {
    if (Value* x = extract_i32_from_sitofp(v)) return x;
    if (v && v->is_instruction()) {
        const Instruction* def = v->defining_instruction();
        if (def) {
            if (def->opcode() == Opcode::fconst_f64 && std::isfinite(def->imm_f64()) &&
                std::fabs(def->imm_f64()) <= 4294967295.0) {
                return b.build_iconst_i32(static_cast<int32_t>(static_cast<int64_t>(def->imm_f64())));
            }
            if ((def->opcode() == Opcode::add || def->opcode() == Opcode::sub) &&
                def->operand_count() == 2) {
                for (int side = 0; side < 2; ++side) {
                    if (side == 1 && def->opcode() == Opcode::sub) break;
                    Value* lhs_i32 = extract_i32_from_sitofp(def->operand(side));
                    const Instruction* rdef =
                        def->operand(1 - side) ? def->operand(1 - side)->defining_instruction() : nullptr;
                    if (lhs_i32 && rdef && rdef->opcode() == Opcode::fconst_f64 &&
                        std::isfinite(rdef->imm_f64()) &&
                        std::floor(rdef->imm_f64()) == rdef->imm_f64() &&
                        std::fabs(rdef->imm_f64()) <= 2147483648.0) {
                        Value* rhs_i32 = b.build_iconst_i32(
                            static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(rdef->imm_f64()))));
                        return def->opcode() == Opcode::add ? b.build_add(lhs_i32, rhs_i32)
                                                            : b.build_sub(lhs_i32, rhs_i32);
                    }
                }
            }
        }
    }
    BasicBlock* cur = b.current_block();
    const std::string pfx = "ta_toi32_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* fast = b.append_block(pfx + "_fast");
    BasicBlock* slow = b.append_block(pfx + "_slow");
    BasicBlock* merge = b.append_block(pfx + "_merge");
    Value* res = b.add_block_param(merge, Type::i32());
    b.position_at_end(cur);
    Value* bits = b.build_bitcast_i64_f64(v);
    Value* abs_bits = b.build_and(bits, iconst(b, 0x7FFFFFFFFFFFFFFFULL));
    b.build_br_if(b.build_ult(abs_bits, iconst(b, 0x43E0000000000000ULL)), fast, slow);
    b.position_at_end(slow);
    Value* in_range = b.build_ult(abs_bits, iconst(b, 0x4530000000000000ULL));
    Value* mant = b.build_or(b.build_and(abs_bits, iconst(b, 0x000FFFFFFFFFFFFFULL)),
                             iconst(b, 0x0010000000000000ULL));
    Value* shift = b.build_and(b.build_sub(b.build_lshr(abs_bits, iconst(b, 52)), iconst(b, 1075)),
                               iconst(b, 63));
    Value* mag = b.build_shl(mant, shift);
    Value* signed_val = b.build_select(b.build_slt(v, b.build_fconst_f64(0.0)), b.build_sub(iconst(b, 0), mag), mag);
    b.build_br(merge, {b.build_trunc_i32(b.build_select(in_range, signed_val, iconst(b, 0)))});
    b.position_at_end(fast);
    b.build_br(merge, {b.build_trunc_i32(b.build_fptosi_i64(v))});
    b.position_at_end(merge);
    return res;
}

} // namespace

Value* PropertyLoweringHelper::lower_elem_get_call(Builder& b, Value* obj, Value* index) {
    return b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)});
}

void PropertyLoweringHelper::lower_elem_set_call(Builder& b, Value* obj, Value* index, Value* val,
                                                 uint32_t strict) {
    b.build_call("bronze_elem_set", Type::void_type(),
                 {obj, boxed_index(b, index), val, b.build_iconst_i32(static_cast<int32_t>(strict))});
}

Value* PropertyLoweringHelper::lower_elem_get(Builder& b, Value* obj, Value* index) {
    if (!enable_inlined_fastpaths_) {
        return b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)});
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_get_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_array = b.append_block(prefix + "_array");
    BasicBlock* bb_array_load = b.append_block(prefix + "_array_load");
    BasicBlock* bb_ta_check = b.append_block(prefix + "_ta_check");
    BasicBlock* bb_ta = b.append_block(prefix + "_ta");
    BasicBlock* bb_ta_load = b.append_block(prefix + "_ta_load");
    BasicBlock* bb_f64 = b.append_block(prefix + "_f64");
    BasicBlock* bb_f32 = b.append_block(prefix + "_f32");
    BasicBlock* bb_i32 = b.append_block(prefix + "_i32");
    BasicBlock* bb_u32 = b.append_block(prefix + "_u32");
    BasicBlock* bb_i16 = b.append_block(prefix + "_i16");
    BasicBlock* bb_u16 = b.append_block(prefix + "_u16");
    BasicBlock* bb_i8 = b.append_block(prefix + "_i8");
    BasicBlock* bb_u8 = b.append_block(prefix + "_u8");
    BasicBlock* bb_box = b.append_block(prefix + "_box");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* number = b.add_block_param(bb_box, Type::f64());
    Value* result = b.add_block_param(bb_merge, Type::i64());

    // An object and an integral index, or the helper.
    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    const ElemIndex idx = elem_index(b, index);
    b.build_br_if(b.build_and(is_obj, idx.ok), bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* header_low = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow)), bb_array, bb_ta_check);

    // An Array: an in-bounds element, a hole reading as undefined (the
    // helper's answer too). Past the length the helper decides.
    b.position_at_end(bb_array);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    b.build_br_if(b.build_ult(idx.i64, length), bb_array_load, bb_slow);

    b.position_at_end(bb_array_load);
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* elem = b.build_load_indexed(Type::i64(), block, b.build_add(idx.i64, head), 8,
                                       static_cast<int32_t>(kBronzeHdrBytes));
    Value* is_hole = b.build_eq(elem, iconst(b, BRONZE_ABI_HOLE_BITS));
    b.build_br(bb_merge, {b.build_select(is_hole, iconst(b, kUndefinedTag), elem)});

    b.position_at_end(bb_ta_check);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kTypedArrayHeaderLow)), bb_ta, bb_slow);

    // A TypedArray of a Number kind, in bounds (a detached view's length is
    // 0); Float16 and the BigInt kinds take the helper.
    b.position_at_end(bb_ta);
    Value* ta_length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
    b.build_br_if(b.build_ult(idx.i64, ta_length), bb_ta_load, bb_slow);

    b.position_at_end(bb_ta_load);
    Value* kind = b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET);
    Value* data = typed_array_data(b, ptr);
    b.build_switch(kind, bb_slow,
                   {SwitchCase(BRONZE_ABI_TA_KIND_FLOAT64, bb_f64), SwitchCase(BRONZE_ABI_TA_KIND_FLOAT32, bb_f32),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT32, bb_i32), SwitchCase(BRONZE_ABI_TA_KIND_UINT32, bb_u32),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT16, bb_i16), SwitchCase(BRONZE_ABI_TA_KIND_UINT16, bb_u16),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT8, bb_i8), SwitchCase(BRONZE_ABI_TA_KIND_UINT8, bb_u8),
                    SwitchCase(BRONZE_ABI_TA_KIND_UINT8CLAMPED, bb_u8)});

    b.position_at_end(bb_f64);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_FLOAT64)});
    b.position_at_end(bb_f32);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_FLOAT32)});
    b.position_at_end(bb_i32);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_INT32)});
    b.position_at_end(bb_u32);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_UINT32)});
    b.position_at_end(bb_i16);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_INT16)});
    b.position_at_end(bb_u16);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_UINT16)});
    b.position_at_end(bb_i8);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_INT8)});
    b.position_at_end(bb_u8);
    b.build_br(bb_box, {load_typed_array_elem(b, data, idx.i64, BRONZE_ABI_TA_KIND_UINT8)});

    b.position_at_end(bb_box);
    b.build_br(bb_merge, {box_f64_bits(b, number)});

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)})});

    b.position_at_end(bb_merge);
    return result;
}

Value* PropertyLoweringHelper::lower_elem_get_typed(Builder& b, Value* obj, Value* index, int64_t elem_kind,
                                                    Value* preloaded_data, Value* preloaded_length) {
    if (!enable_inlined_fastpaths_) {
        return b.build_bitcast_f64_i64(b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)}));
    }

    const bool proven_integral = (elem_kind & (1LL << 16)) != 0;
    const bool proven_in_bounds = (elem_kind & (1LL << 17)) != 0;
    elem_kind &= 0xFFFF;
    constexpr int64_t kElemKindPlainArrayF64 = 100;
    if (elem_kind == kElemKindPlainArrayF64) {
        BasicBlock* cur = b.current_block();
        Function* fn = cur->parent();
        const std::string prefix = "elem_get_arr_" + std::to_string(fn->next_block_id());
        BasicBlock* bb_obj = b.append_block(prefix + "_obj");
        BasicBlock* bb_array = b.append_block(prefix + "_array");
        BasicBlock* bb_array_load = b.append_block(prefix + "_array_load");
        BasicBlock* bb_slow = b.append_block(prefix + "_slow");
        BasicBlock* bb_merge = b.append_block(prefix + "_merge");
        Value* result = b.add_block_param(bb_merge, Type::i64());

        b.position_at_end(cur);
        Value* ptr = nullptr;
        Value* is_obj = nullptr;
        object_pointer(b, obj, ptr, is_obj);
        const ElemIndex idx = elem_index(b, index, proven_integral);
        b.build_br_if(b.build_and(is_obj, idx.ok), bb_obj, bb_slow);

        b.position_at_end(bb_obj);
        Value* header_low = b.build_load(Type::i32(), ptr, 0);
        b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow)), bb_array, bb_slow);

        b.position_at_end(bb_array);
        Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
        b.build_br_if(b.build_ult(idx.i64, length), bb_array_load, bb_slow);

        b.position_at_end(bb_array_load);
        Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
        Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        Value* elem = b.build_load_indexed(Type::i64(), block, b.build_add(idx.i64, head), 8,
                                           static_cast<int32_t>(kBronzeHdrBytes));
        Value* is_hole = b.build_eq(elem, iconst(b, BRONZE_ABI_HOLE_BITS));
        b.build_br(bb_merge, {b.build_select(is_hole, iconst(b, kUndefinedTag), elem)});

        b.position_at_end(bb_slow);
        b.build_br(bb_merge, {b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)})});

        b.position_at_end(bb_merge);
        return b.build_bitcast_f64_i64(result);
    }

    static const bool unsound_pins = [] {
        const char* env = std::getenv("BRONZE_UNSOUND_PINS");
        return env && env[0] && env[0] != '0';
    }();
    if (unsound_pins || elem_kind < BRONZE_ABI_TA_KIND_INT8 || elem_kind > BRONZE_ABI_TA_KIND_FLOAT64) {
        return b.build_bitcast_f64_i64(lower_elem_get(b, obj, index));
    }

    const bool known_num_idx =
        index->type() == Type::i32() || index->type() == Type::f64() || boxed_f64_source(index) != nullptr;
    const ElemIndex idx = elem_index(b, index, proven_integral);
    if (proven_in_bounds && proven_integral && known_num_idx) {
        Value* data = preloaded_data ? preloaded_data
                                     : typed_array_data(b, b.build_and(obj, iconst(b, kPayloadMask)));
        return load_typed_array_elem(b, data, idx.i64, elem_kind);
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_get_ta_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_load = b.append_block(prefix + "_load");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::f64());

    b.position_at_end(cur);
    Value* ptr = (preloaded_data && preloaded_length) ? nullptr : b.build_and(obj, iconst(b, kPayloadMask));
    Value* ta_len = preloaded_length
                        ? preloaded_length
                        : b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
    Value* in_bounds = b.build_ult(idx.i64, ta_len);
    Value* ok = (proven_integral && known_num_idx) ? in_bounds : b.build_and(idx.ok, in_bounds);
    Value* nan_f64 = b.build_bitcast_f64_i64(iconst(b, BRONZE_ABI_CANONICAL_NAN_BITS));
    b.build_br_if(ok, bb_load, {}, bb_merge, {nan_f64});

    b.position_at_end(bb_load);
    Value* data = preloaded_data ? preloaded_data : typed_array_data(b, ptr);
    Value* loaded_val = load_typed_array_elem(b, data, idx.i64, elem_kind);
    b.build_br(bb_merge, {loaded_val});

    b.position_at_end(bb_merge);
    last_ta_get_ = {bb_merge, bb_load, result, loaded_val, data, idx.i64, obj, index, elem_kind};
    return result;
}

// The inline stores are exactly the helper's own fast path
// (rt_prop_write.cpp bronze_elem_set), so they decide nothing it does not:
//
//  - An Array element at an index below both its length and capacity, on an
//    array with no named-properties object. Frozen, sealed and non-writable
//    elements and accessor elements all live behind that object, so an array
//    without one has none of them. The store is followed by the write barrier
//    for a reference value, on the slot's own address, as HeapValue gives it.
//  - A TypedArray element in bounds, of a Number value, for the Number kinds
//    Int8..Float64. An integer kind stores the low bits of the value's ToInt32
//    (inline for a magnitude below 2^63, as the bitwise operators take it: the
//    truncation's low 8/16/32 bits are ToInt8..ToUint32 alike); Uint8Clamped
//    only an integral value, clamped; Float32 the rounded float. Everything
//    else — Float16, the BigInt kinds, a non-Number value, NaN or a huge
//    magnitude into an integer kind, a fraction into Uint8Clamped, any index
//    out of bounds (a discard the helper makes) — takes the helper.
void PropertyLoweringHelper::lower_elem_set(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict) {
    Value* strict_val = b.build_iconst_i32(static_cast<int32_t>(strict));
    if (!enable_inlined_fastpaths_) {
        b.build_call("bronze_elem_set", Type::void_type(), {obj, boxed_index(b, index), val, strict_val});
        return;
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_set_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_array = b.append_block(prefix + "_array");
    BasicBlock* bb_array_store = b.append_block(prefix + "_array_store");
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_ta_check = b.append_block(prefix + "_ta_check");
    BasicBlock* bb_ta = b.append_block(prefix + "_ta");
    BasicBlock* bb_ta_store = b.append_block(prefix + "_ta_store");
    BasicBlock* bb_f64 = b.append_block(prefix + "_f64");
    BasicBlock* bb_f32 = b.append_block(prefix + "_f32");
    BasicBlock* bb_int_check = b.append_block(prefix + "_int_check");
    BasicBlock* bb_int = b.append_block(prefix + "_int");
    BasicBlock* bb_i32 = b.append_block(prefix + "_i32");
    BasicBlock* bb_i16 = b.append_block(prefix + "_i16");
    BasicBlock* bb_i8 = b.append_block(prefix + "_i8");
    BasicBlock* bb_clamp_check = b.append_block(prefix + "_clamp_check");
    BasicBlock* bb_clamp = b.append_block(prefix + "_clamp");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    // An object and an integral index, or the helper.
    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    const ElemIndex idx = elem_index(b, index);
    b.build_br_if(b.build_and(is_obj, idx.ok), bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* header_low = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow)), bb_array, bb_ta_check);

    // An Array: in bounds of length and capacity, and no properties object.
    b.position_at_end(bb_array);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    Value* capacity = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
    Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
    Value* slot = b.build_add(idx.i64, head);
    Value* props = b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    Value* in_bounds = b.build_and(b.build_ult(idx.i64, length), b.build_ult(slot, capacity));
    b.build_br_if(b.build_and(in_bounds, no_props), bb_array_store, bb_slow);

    b.position_at_end(bb_array_store);
    Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    b.build_store_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes), val);
    // Every reference is tagged above the Number range.
    b.build_br_if(b.build_ugt(val, iconst(b, kBronzeNumberMaxBits)), bb_barrier, bb_merge);

    b.position_at_end(bb_barrier);
    Value* slot_addr = b.build_add(b.build_add(block, b.build_shl(slot, iconst(b, 3))), iconst(b, kBronzeHdrBytes));
    b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
    b.build_br(bb_merge);

    // A TypedArray of a Number kind, in bounds, and a Number value.
    b.position_at_end(bb_ta_check);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kTypedArrayHeaderLow)), bb_ta, bb_slow);

    b.position_at_end(bb_ta);
    Value* num = boxed_f64_source(val);
    Value* is_number = num ? b.build_iconst_i32(1) : b.build_ule(val, iconst(b, kBronzeNumberMaxBits));
    if (!num) num = b.build_bitcast_f64_i64(val);
    Value* ta_length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
    b.build_br_if(b.build_and(is_number, b.build_ult(idx.i64, ta_length)), bb_ta_store, bb_slow);

    b.position_at_end(bb_ta_store);
    Value* kind = b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET);
    Value* data = typed_array_data(b, ptr);
    b.build_switch(kind, bb_slow,
                   {SwitchCase(BRONZE_ABI_TA_KIND_FLOAT64, bb_f64), SwitchCase(BRONZE_ABI_TA_KIND_FLOAT32, bb_f32),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT32, bb_int_check), SwitchCase(BRONZE_ABI_TA_KIND_UINT32, bb_int_check),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT16, bb_int_check), SwitchCase(BRONZE_ABI_TA_KIND_UINT16, bb_int_check),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT8, bb_int_check), SwitchCase(BRONZE_ABI_TA_KIND_UINT8, bb_int_check),
                    SwitchCase(BRONZE_ABI_TA_KIND_UINT8CLAMPED, bb_clamp_check)});

    b.position_at_end(bb_f64);
    b.build_store_indexed(Type::f64(), data, idx.i64, 8, num);
    b.build_br(bb_merge);
    b.position_at_end(bb_f32);
    b.build_store_indexed(Type::f32(), data, idx.i64, 4, b.build_fptrunc_f32_f64(num));
    b.build_br(bb_merge);

    // The integer kinds: a magnitude below 2^63 (NaN and the infinities fail
    // it) truncates exactly, and its low bits are the stored element.
    b.position_at_end(bb_int_check);
    Value* abs_bits = b.build_and(b.build_bitcast_i64_f64(num), iconst(b, 0x7FFFFFFFFFFFFFFFULL));
    b.build_br_if(b.build_ult(abs_bits, iconst(b, 0x43E0000000000000ULL)), bb_int, bb_slow);

    b.position_at_end(bb_int);
    Value* whole = b.build_fptosi_i64(num);
    b.build_switch(kind, bb_slow,
                   {SwitchCase(BRONZE_ABI_TA_KIND_INT32, bb_i32), SwitchCase(BRONZE_ABI_TA_KIND_UINT32, bb_i32),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT16, bb_i16), SwitchCase(BRONZE_ABI_TA_KIND_UINT16, bb_i16),
                    SwitchCase(BRONZE_ABI_TA_KIND_INT8, bb_i8), SwitchCase(BRONZE_ABI_TA_KIND_UINT8, bb_i8)});

    b.position_at_end(bb_i32);
    b.build_store_indexed(Type::i32(), data, idx.i64, 4, b.build_trunc_i32(whole));
    b.build_br(bb_merge);
    // Two bytes, little-endian, as load_u16 reads them.
    b.position_at_end(bb_i16);
    b.build_store_indexed(Type::i8(), data, idx.i64, 2, 0, b.build_trunc_i8(whole));
    b.build_store_indexed(Type::i8(), data, idx.i64, 2, 1, b.build_trunc_i8(b.build_lshr(whole, iconst(b, 8))));
    b.build_br(bb_merge);
    b.position_at_end(bb_i8);
    b.build_store_indexed(Type::i8(), data, idx.i64, 1, b.build_trunc_i8(whole));
    b.build_br(bb_merge);

    // Uint8Clamped: an integral value (the round trip is exact, so no
    // rounding is owed and NaN fails it), clamped to 0..255.
    b.position_at_end(bb_clamp_check);
    Value* c_abs = b.build_and(b.build_bitcast_i64_f64(num), iconst(b, 0x7FFFFFFFFFFFFFFFULL));
    Value* c_whole = b.build_fptosi_i64(num);
    Value* c_exact = b.build_eq(b.build_sitofp_f64_i64(c_whole), num);
    b.build_br_if(b.build_and(b.build_ult(c_abs, iconst(b, 0x43E0000000000000ULL)), c_exact), bb_clamp, bb_slow);

    b.position_at_end(bb_clamp);
    Value* lo = b.build_select(b.build_slt(c_whole, iconst(b, 0)), iconst(b, 0), c_whole);
    Value* clamped = b.build_select(b.build_sgt(lo, iconst(b, 255)), iconst(b, 255), lo);
    b.build_store_indexed(Type::i8(), data, idx.i64, 1, b.build_trunc_i8(clamped));
    b.build_br(bb_merge);

    b.position_at_end(bb_slow);
    b.build_call("bronze_elem_set", Type::void_type(), {obj, boxed_index(b, index), val, strict_val});
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

void PropertyLoweringHelper::lower_elem_set_typed(Builder& b, Value* obj, Value* index, Value* val_f64,
                                                  int64_t elem_kind, uint32_t strict, Value* preloaded_data,
                                                  Value* preloaded_length) {
    Value* strict_val = b.build_iconst_i32(static_cast<int32_t>(strict));
    if (!enable_inlined_fastpaths_) {
        b.build_call("bronze_elem_set", Type::void_type(),
                     {obj, boxed_index(b, index), box_f64_bits(b, val_f64), strict_val});
        return;
    }

    const bool proven_integral = (elem_kind & (1LL << 16)) != 0;
    const bool proven_in_bounds = (elem_kind & (1LL << 17)) != 0;
    elem_kind &= 0xFFFF;
    constexpr int64_t kElemKindPlainArrayF64 = 100;
    if (elem_kind == kElemKindPlainArrayF64) {
        BasicBlock* cur = b.current_block();
        Function* fn = cur->parent();
        const std::string prefix = "elem_set_arr_" + std::to_string(fn->next_block_id());
        BasicBlock* bb_obj = b.append_block(prefix + "_obj");
        BasicBlock* bb_array = b.append_block(prefix + "_array");
        BasicBlock* bb_array_store = b.append_block(prefix + "_array_store");
        BasicBlock* bb_slow = b.append_block(prefix + "_slow");
        BasicBlock* bb_merge = b.append_block(prefix + "_merge");

        b.position_at_end(cur);
        Value* ptr = nullptr;
        Value* is_obj = nullptr;
        object_pointer(b, obj, ptr, is_obj);
        const ElemIndex idx = elem_index(b, index, proven_integral);
        b.build_br_if(b.build_and(is_obj, idx.ok), bb_obj, bb_slow);

        b.position_at_end(bb_obj);
        Value* header_low = b.build_load(Type::i32(), ptr, 0);
        b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow)), bb_array, bb_slow);

        b.position_at_end(bb_array);
        Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
        Value* capacity = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
        Value* head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        Value* slot = b.build_add(idx.i64, head);
        Value* props = b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
        Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
        Value* in_bounds = b.build_and(b.build_ult(idx.i64, length), b.build_ult(slot, capacity));
        b.build_br_if(b.build_and(in_bounds, no_props), bb_array_store, bb_slow);

        b.position_at_end(bb_array_store);
        Value* block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
        // A boxed f64 is a primitive Number, never a heap reference, so no write barrier is needed.
        b.build_store_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes), box_f64_bits(b, val_f64));
        b.build_br(bb_merge);

        b.position_at_end(bb_slow);
        b.build_call("bronze_elem_set", Type::void_type(),
                     {obj, boxed_index(b, index), box_f64_bits(b, val_f64), strict_val});
        b.build_br(bb_merge);

        b.position_at_end(bb_merge);
        return;
    }

    static const bool unsound_pins = [] {
        const char* env = std::getenv("BRONZE_UNSOUND_PINS");
        return env && env[0] && env[0] != '0';
    }();
    if (unsound_pins || elem_kind < BRONZE_ABI_TA_KIND_INT8 || elem_kind > BRONZE_ABI_TA_KIND_FLOAT64) {
        lower_elem_set(b, obj, index, box_f64_bits(b, val_f64), strict);
        return;
    }

    const LastTypedGet prev_get = last_ta_get_;
    last_ta_get_ = {};
    auto emit_typed_store = [&](Value* data, Value* idx_i64) {
        switch (elem_kind) {
            case BRONZE_ABI_TA_KIND_FLOAT64:
                b.build_store_indexed(Type::f64(), data, idx_i64, 8, val_f64);
                break;
            case BRONZE_ABI_TA_KIND_FLOAT32:
                b.build_store_indexed(Type::f32(), data, idx_i64, 4, b.build_fptrunc_f32_f64(val_f64));
                break;
            case BRONZE_ABI_TA_KIND_INT32:
            case BRONZE_ABI_TA_KIND_UINT32:
                b.build_store_indexed(Type::i32(), data, idx_i64, 4, narrow_f64_to_i32_callfree(b, val_f64));
                break;
            case BRONZE_ABI_TA_KIND_INT16:
            case BRONZE_ABI_TA_KIND_UINT16: {
                Value* i32 = narrow_f64_to_i32_callfree(b, val_f64);
                b.build_store_indexed(Type::i8(), data, idx_i64, 2, 0, b.build_trunc_i8(i32));
                b.build_store_indexed(Type::i8(), data, idx_i64, 2, 1,
                                      b.build_trunc_i8(b.build_lshr(i32, b.build_iconst_i32(8))));
                break;
            }
            case BRONZE_ABI_TA_KIND_INT8:
            case BRONZE_ABI_TA_KIND_UINT8:
                b.build_store_indexed(Type::i8(), data, idx_i64, 1,
                                      b.build_trunc_i8(narrow_f64_to_i32_callfree(b, val_f64)));
                break;
            default: { // UINT8CLAMPED: half-to-even rounding in [0, 255]
                Value* is_nan = b.build_ne(val_f64, val_f64);
                Value* le_zero = b.build_or(is_nan, b.build_sle(val_f64, b.build_fconst_f64(0.0)));
                Value* ge_255 = b.build_sge(val_f64, b.build_fconst_f64(255.0));
                Value* trunc_i64 = b.build_fptosi_i64(val_f64);
                Value* floor_f64 = b.build_sitofp_f64_i64(trunc_i64);
                Value* diff = b.build_sub(val_f64, floor_f64);
                Value* gt_half = b.build_sgt(diff, b.build_fconst_f64(0.5));
                Value* eq_half = b.build_eq(diff, b.build_fconst_f64(0.5));
                Value* is_odd = b.build_ne(b.build_and(trunc_i64, iconst(b, 1)), iconst(b, 0));
                Value* round_up = b.build_or(gt_half, b.build_and(eq_half, is_odd));
                Value* rounded = b.build_add(trunc_i64, b.build_zext_i64(round_up));
                Value* clamped = b.build_select(le_zero, iconst(b, 0),
                                                b.build_select(ge_255, iconst(b, 255), rounded));
                b.build_store_indexed(Type::i8(), data, idx_i64, 1, b.build_trunc_i8(clamped));
                break;
            }
        }
    };

    if (b.current_block() == prev_get.merge_bb && prev_get.load_bb && prev_get.load_bb->tail() &&
        obj == prev_get.obj && index == prev_get.index && elem_kind == prev_get.elem_kind &&
        (elem_kind == BRONZE_ABI_TA_KIND_FLOAT64 || elem_kind == BRONZE_ABI_TA_KIND_FLOAT32 ||
         extract_i32_from_sitofp(prev_get.loaded_val) != nullptr) &&
        val_f64 && val_f64->is_instruction()) {
        const Instruction* vdef = val_f64->defining_instruction();
        const Instruction* rdef = (vdef && vdef->operand_count() == 2 && vdef->operand(1))
                                      ? vdef->operand(1)->defining_instruction()
                                      : nullptr;
        if (vdef && vdef->parent() == prev_get.merge_bb &&
            (vdef->opcode() == Opcode::add || vdef->opcode() == Opcode::sub) &&
            vdef->operand(0) == prev_get.result && rdef && rdef->opcode() == Opcode::fconst_f64 &&
            std::isfinite(rdef->imm_f64()) && std::floor(rdef->imm_f64()) == rdef->imm_f64() &&
            std::fabs(rdef->imm_f64()) <= 2147483648.0) {
            bool pure_merge = true;
            for (const Instruction* it = prev_get.merge_bb->head(); it; it = it->next()) {
                if (it->opcode() != Opcode::fconst_f64 && it->opcode() != Opcode::add &&
                    it->opcode() != Opcode::sub && it->opcode() != Opcode::iconst_i32 &&
                    it->opcode() != Opcode::iconst_i64) {
                    pure_merge = false;
                    break;
                }
            }
            if (pure_merge) {
                BasicBlock* resume = b.current_block();
                b.position_before(prev_get.load_bb->tail());
                Value* c = b.build_fconst_f64(rdef->imm_f64());
                val_f64 = vdef->opcode() == Opcode::add ? b.build_add(prev_get.loaded_val, c)
                                                        : b.build_sub(prev_get.loaded_val, c);
                emit_typed_store(prev_get.data, prev_get.idx_i64);
                b.position_at_end(resume);
                return;
            }
        }
    }

    const bool known_num_idx =
        index->type() == Type::i32() || index->type() == Type::f64() || boxed_f64_source(index) != nullptr;
    const ElemIndex idx = elem_index(b, index, proven_integral);
    if (proven_in_bounds && proven_integral && known_num_idx) {
        Value* data = preloaded_data ? preloaded_data
                                     : typed_array_data(b, b.build_and(obj, iconst(b, kPayloadMask)));
        emit_typed_store(data, idx.i64);
        return;
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_set_ta_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(cur);
    Value* ptr = (preloaded_data && preloaded_length) ? nullptr : b.build_and(obj, iconst(b, kPayloadMask));
    Value* ta_len = preloaded_length
                        ? preloaded_length
                        : b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
    Value* in_bounds = b.build_ult(idx.i64, ta_len);
    Value* ok = (proven_integral && known_num_idx) ? in_bounds : b.build_and(idx.ok, in_bounds);
    b.build_br_if(ok, bb_store, bb_merge);

    b.position_at_end(bb_store);
    Value* data = preloaded_data ? preloaded_data : typed_array_data(b, ptr);
    emit_typed_store(data, idx.i64);
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
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

void PropertyLoweringHelper::emit_dense_array_guard(Builder& b, Value* obj, uint32_t max_index, bool writable,
                                                    Value*& out_ok, Value*& out_block, Value*& out_head) {
    const bool can_select = in_process_ || (b.current_module() && b.current_module()->pinned_tls_register());
    auto emit_header_checks = [&](Value* base, Value* is_obj_val, Value*& ok, Value*& block, Value*& head) {
        Value* hdr_ok = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kArrayHeaderLow));
        Value* is_array = is_obj_val ? b.build_and(is_obj_val, hdr_ok) : hdr_ok;
        Value* elems = b.build_load(Type::i64(), base, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
        Value* has_elems = b.build_eq(b.build_and(elems, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
        Value* raw_block = b.build_and(elems, iconst(b, kPayloadMask));
        Value* raw_head = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        block = b.build_add(raw_block, b.build_shl(raw_head, iconst(b, 3)));
        head = nullptr;
        if (!writable) {
            Value* len = b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
            Value* in_bounds = b.build_ult(b.build_iconst_i32(static_cast<int32_t>(max_index)), len);
            ok = b.build_and(is_array, b.build_and(in_bounds, has_elems));
        } else {
            Value* len = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
            Value* cap = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
            Value* max_slot = max_index == 0 ? raw_head : b.build_add(raw_head, iconst(b, max_index));
            Value* in_bounds = b.build_and(b.build_ult(iconst(b, max_index), len), b.build_ult(max_slot, cap));
            Value* props = b.build_load(Type::i64(), base, BRONZE_ABI_ARRAY_PROPS_OFFSET);
            Value* no_props = b.build_ne(b.build_and(props, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
            ok = b.build_and(b.build_and(is_array, no_props), b.build_and(in_bounds, has_elems));
        }
    };

    if (can_select) {
        Value* ptr = b.build_xor(obj, iconst(b, kObjectTagBits));
        Value* is_obj = b.build_ult(ptr, iconst(b, kPayloadLimit));
        Value* not_arr = in_process_
            ? iconst(b, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&kNotAnArray[0])))
            : b.build_pinned_tls_read();
        Value* base = b.build_select(is_obj, ptr, not_arr);
        // Either stand-in's first word is no header (the zeroed block, or the
        // TLS block's even proto epoch: bronze_abi.h), so the header compare
        // alone refuses a non-object.
        emit_header_checks(base, nullptr, out_ok, out_block, out_head);
        return;
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "dense_guard_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    out_ok = b.add_block_param(bb_merge, Type::i32());
    out_block = b.add_block_param(bb_merge, Type::i64());
    out_head = nullptr;

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    b.build_br_if(is_obj, bb_arr, {}, bb_merge, {b.build_iconst_i32(0), iconst(b, 0)});

    b.position_at_end(bb_arr);
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_header_checks(ptr, nullptr, ok, block, head);
    b.build_br(bb_merge, {ok, block});

    b.position_at_end(bb_merge);
}

Value* PropertyLoweringHelper::lower_is_dense_array(Builder& b, Value* obj, uint32_t max_index) {
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_dense_array_guard(b, obj, max_index, /*writable=*/false, ok, block, head);
    return ok;
}

// Branches rather than selects over a stand-in block: the test is made once
// per entry of the loop it versions, never per element. The kind word is read
// only once the header says the object is a view, since only a view is known
// to be that long.
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
    Value* kind_ok = b.build_eq(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET),
                                b.build_iconst_i32(static_cast<int32_t>(elem_kind)));
    if (pristine) {
        kind_ok = b.build_and(kind_ok, typed_array_chain_pristine(b, ptr, iconst(b, static_cast<uint64_t>(elem_kind))));
    }
    b.build_br(bb_merge, {kind_ok});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::emit_elem_index_guard(Builder& b, Value* index, Value*& out_i64, Value*& out_ok) {
    const ElemIndex idx = elem_index(b, index);
    out_i64 = idx.i64;
    out_ok = idx.ok;
}

Value* PropertyLoweringHelper::unbox_known_number(Builder& b, Value* val, Value*& out_ok) {
    out_ok = nullptr;
    if (val->type() == Type::f64()) return val;
    if (Value* src = boxed_f64_source(val)) return src;
    out_ok = b.build_ule(val, iconst(b, kBronzeNumberMaxBits));
    return b.build_bitcast_f64_i64(val);
}

Value* PropertyLoweringHelper::box_number(Builder& b, Value* f64_val) {
    return box_f64_bits(b, f64_val);
}

void PropertyLoweringHelper::object_payload(Builder& b, Value* obj, Value*& out_ptr, Value*& out_is_obj) {
    object_pointer(b, obj, out_ptr, out_is_obj);
}

Value* PropertyLoweringHelper::load_typed_array_data(Builder& b, Value* ptr) {
    return typed_array_data(b, ptr);
}

} // namespace il2mir
