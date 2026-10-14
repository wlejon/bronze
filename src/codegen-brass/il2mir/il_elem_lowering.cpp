// Keyed element reads and writes (`o[i]`): the inline paths for an Array or a
// TypedArray read or written at an integral in-bounds index, and the runtime
// helpers (bronze_elem_get / bronze_elem_set) for everything else.
//
// The layouts are the runtime's (runtime/array.h, runtime/typed_array.h),
// pinned by the BRONZE_ABI_* offsets their static_asserts check.

#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kTagMask = 0xFFFF000000000000ULL;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint64_t kPayloadLimit = uint64_t{1} << 48;
constexpr uint64_t kMaxInlineDispIndex = (uint64_t{INT32_MAX} - kBronzeHdrBytes) / 8;
// The header word's low half: the object tag, then HeapObjectHeader::flags.
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kTypedArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

// A non-object's header and array words, for an in-process module: the zeroed
// block's header is no Array's, so branchless header/length/elements loads
// through it safely refuse non-objects.
alignas(16) const uint64_t kNotAnArray[8] = {};

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

// An f64 as a Value's bits, every NaN the canonical one.
Value* box_f64(Builder& b, Value* f) {
    Value* bits = b.build_bitcast_i64_f64(f);
    Value* abs_bits = b.build_and(bits, iconst(b, 0x7FFFFFFFFFFFFFFFULL));
    Value* is_nan = b.build_ugt(abs_bits, iconst(b, 0x7FF0000000000000ULL));
    return b.build_select(is_nan, iconst(b, BRONZE_ABI_CANONICAL_NAN_BITS), bits);
}

// An element index: `i64` is the index when `ok` (an integral Number; the
// element paths range-check it against an unsigned length, so a negative one
// misses there). The boxed Value for the slow-path helper is materialized only
// in `bb_slow`.
struct ElemIndex {
    Value* i64 = nullptr;
    Value* ok = nullptr;
};

// The f64 an i64 Value was boxed from here (box_f64, IlLowering::ensure_type:
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
    if (index->type() == Type::f64()) return box_f64(b, index);
    return index;
}

ElemIndex elem_index(Builder& b, Value* index) {
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
    // Integral when the round trip is exact (the compare is ordered: NaN
    // fails it; -0 reads as index 0, as ToPropertyKey makes it "0").
    r.i64 = b.build_fptosi_i64(f);
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
    b.build_br(bb_box, {b.build_load_indexed(Type::f64(), data, idx.i64, 8)});
    b.position_at_end(bb_f32);
    b.build_br(bb_box, {b.build_fpext_f64_f32(b.build_load_indexed(Type::f32(), data, idx.i64, 4))});
    b.position_at_end(bb_i32);
    b.build_br(bb_box, {b.build_sitofp_f64_i32(b.build_load_indexed(Type::i32(), data, idx.i64, 4))});
    b.position_at_end(bb_u32);
    b.build_br(bb_box,
               {b.build_sitofp_f64_i64(b.build_zext_i64(b.build_load_indexed(Type::i32(), data, idx.i64, 4)))});
    b.position_at_end(bb_i16);
    b.build_br(bb_box, {b.build_sitofp_f64_i64(sign_extend(b, load_u16(b, data, idx.i64), 16))});
    b.position_at_end(bb_u16);
    b.build_br(bb_box, {b.build_sitofp_f64_i64(load_u16(b, data, idx.i64))});
    b.position_at_end(bb_i8);
    b.build_br(bb_box, {b.build_sitofp_f64_i64(
                           sign_extend(b, b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx.i64, 1)), 8))});
    b.position_at_end(bb_u8);
    b.build_br(bb_box,
               {b.build_sitofp_f64_i64(b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx.i64, 1)))});

    b.position_at_end(bb_box);
    b.build_br(bb_merge, {box_f64(b, number)});

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)})});

    b.position_at_end(bb_merge);
    return result;
}

Value* PropertyLoweringHelper::lower_elem_get_typed(Builder& b, Value* obj, Value* index, int64_t elem_kind) {
    if (!enable_inlined_fastpaths_) {
        return b.build_bitcast_f64_i64(b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)}));
    }

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
        const ElemIndex idx = elem_index(b, index);
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

    if (elem_kind < BRONZE_ABI_TA_KIND_INT8 || elem_kind > BRONZE_ABI_TA_KIND_FLOAT64) {
        return b.build_bitcast_f64_i64(lower_elem_get(b, obj, index));
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_get_ta_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_ta = b.append_block(prefix + "_ta");
    BasicBlock* bb_load = b.append_block(prefix + "_load");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::f64());

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    const ElemIndex idx = elem_index(b, index);
    b.build_br_if(b.build_and(is_obj, idx.ok), bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* header_low = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kTypedArrayHeaderLow)), bb_ta, bb_slow);

    b.position_at_end(bb_ta);
    Value* ta_length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
    Value* kind = b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET);
    Value* in_bounds = b.build_ult(idx.i64, ta_length);
    Value* kind_ok = b.build_eq(kind, b.build_iconst_i32(static_cast<int32_t>(elem_kind)));
    b.build_br_if(b.build_and(in_bounds, kind_ok), bb_load, bb_slow);

    b.position_at_end(bb_load);
    Value* data = typed_array_data(b, ptr);
    Value* number = nullptr;
    switch (elem_kind) {
        case BRONZE_ABI_TA_KIND_FLOAT64:
            number = b.build_load_indexed(Type::f64(), data, idx.i64, 8);
            break;
        case BRONZE_ABI_TA_KIND_FLOAT32:
            number = b.build_fpext_f64_f32(b.build_load_indexed(Type::f32(), data, idx.i64, 4));
            break;
        case BRONZE_ABI_TA_KIND_INT32:
            number = b.build_sitofp_f64_i32(b.build_load_indexed(Type::i32(), data, idx.i64, 4));
            break;
        case BRONZE_ABI_TA_KIND_UINT32:
            number = b.build_sitofp_f64_i64(b.build_zext_i64(b.build_load_indexed(Type::i32(), data, idx.i64, 4)));
            break;
        case BRONZE_ABI_TA_KIND_INT16:
            number = b.build_sitofp_f64_i64(sign_extend(b, load_u16(b, data, idx.i64), 16));
            break;
        case BRONZE_ABI_TA_KIND_UINT16:
            number = b.build_sitofp_f64_i64(load_u16(b, data, idx.i64));
            break;
        case BRONZE_ABI_TA_KIND_INT8:
            number = b.build_sitofp_f64_i64(
                sign_extend(b, b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx.i64, 1)), 8));
            break;
        default: // UINT8, UINT8CLAMPED
            number = b.build_sitofp_f64_i64(b.build_zext_i64(b.build_load_indexed(Type::i8(), data, idx.i64, 1)));
            break;
    }
    b.build_br(bb_merge, {number});

    b.position_at_end(bb_slow);
    Value* slow_val = b.build_call("bronze_elem_get", Type::i64(), {obj, boxed_index(b, index)});
    b.build_br(bb_merge, {b.build_bitcast_f64_i64(slow_val)});

    b.position_at_end(bb_merge);
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
                                                  int64_t elem_kind, uint32_t strict) {
    Value* strict_val = b.build_iconst_i32(static_cast<int32_t>(strict));
    if (!enable_inlined_fastpaths_) {
        b.build_call("bronze_elem_set", Type::void_type(),
                     {obj, boxed_index(b, index), box_f64(b, val_f64), strict_val});
        return;
    }

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
        const ElemIndex idx = elem_index(b, index);
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
        b.build_store_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes), box_f64(b, val_f64));
        b.build_br(bb_merge);

        b.position_at_end(bb_slow);
        b.build_call("bronze_elem_set", Type::void_type(),
                     {obj, boxed_index(b, index), box_f64(b, val_f64), strict_val});
        b.build_br(bb_merge);

        b.position_at_end(bb_merge);
        return;
    }

    if (elem_kind < BRONZE_ABI_TA_KIND_INT8 || elem_kind > BRONZE_ABI_TA_KIND_FLOAT64) {
        lower_elem_set(b, obj, index, box_f64(b, val_f64), strict);
        return;
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_set_ta_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_ta = b.append_block(prefix + "_ta");
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    const ElemIndex idx = elem_index(b, index);
    b.build_br_if(b.build_and(is_obj, idx.ok), bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* header_low = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kTypedArrayHeaderLow)), bb_ta, bb_slow);

    b.position_at_end(bb_ta);
    Value* ta_length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET));
    Value* kind = b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET);
    Value* ok = b.build_and(b.build_ult(idx.i64, ta_length),
                            b.build_eq(kind, b.build_iconst_i32(static_cast<int32_t>(elem_kind))));
    Value* whole = nullptr;
    if (elem_kind == BRONZE_ABI_TA_KIND_UINT8CLAMPED) {
        Value* c_abs = b.build_and(b.build_bitcast_i64_f64(val_f64), iconst(b, 0x7FFFFFFFFFFFFFFFULL));
        whole = b.build_fptosi_i64(val_f64);
        Value* c_exact = b.build_eq(b.build_sitofp_f64_i64(whole), val_f64);
        Value* c_ok = b.build_and(b.build_ult(c_abs, iconst(b, 0x43E0000000000000ULL)), c_exact);
        ok = b.build_and(ok, c_ok);
    } else if (elem_kind != BRONZE_ABI_TA_KIND_FLOAT64 && elem_kind != BRONZE_ABI_TA_KIND_FLOAT32) {
        Value* abs_bits = b.build_and(b.build_bitcast_i64_f64(val_f64), iconst(b, 0x7FFFFFFFFFFFFFFFULL));
        ok = b.build_and(ok, b.build_ult(abs_bits, iconst(b, 0x43E0000000000000ULL)));
    }
    b.build_br_if(ok, bb_store, bb_slow);

    b.position_at_end(bb_store);
    Value* data = typed_array_data(b, ptr);
    switch (elem_kind) {
        case BRONZE_ABI_TA_KIND_FLOAT64:
            b.build_store_indexed(Type::f64(), data, idx.i64, 8, val_f64);
            break;
        case BRONZE_ABI_TA_KIND_FLOAT32:
            b.build_store_indexed(Type::f32(), data, idx.i64, 4, b.build_fptrunc_f32_f64(val_f64));
            break;
        case BRONZE_ABI_TA_KIND_INT32:
        case BRONZE_ABI_TA_KIND_UINT32:
            whole = b.build_fptosi_i64(val_f64);
            b.build_store_indexed(Type::i32(), data, idx.i64, 4, b.build_trunc_i32(whole));
            break;
        case BRONZE_ABI_TA_KIND_INT16:
        case BRONZE_ABI_TA_KIND_UINT16:
            whole = b.build_fptosi_i64(val_f64);
            b.build_store_indexed(Type::i8(), data, idx.i64, 2, 0, b.build_trunc_i8(whole));
            b.build_store_indexed(Type::i8(), data, idx.i64, 2, 1, b.build_trunc_i8(b.build_lshr(whole, iconst(b, 8))));
            break;
        case BRONZE_ABI_TA_KIND_INT8:
        case BRONZE_ABI_TA_KIND_UINT8:
            whole = b.build_fptosi_i64(val_f64);
            b.build_store_indexed(Type::i8(), data, idx.i64, 1, b.build_trunc_i8(whole));
            break;
        default: { // UINT8CLAMPED
            Value* lo = b.build_select(b.build_slt(whole, iconst(b, 0)), iconst(b, 0), whole);
            Value* clamped = b.build_select(b.build_sgt(lo, iconst(b, 255)), iconst(b, 255), lo);
            b.build_store_indexed(Type::i8(), data, idx.i64, 1, b.build_trunc_i8(clamped));
            break;
        }
    }
    b.build_br(bb_merge);

    b.position_at_end(bb_slow);
    b.build_call("bronze_elem_set", Type::void_type(),
                 {obj, boxed_index(b, index), box_f64(b, val_f64), strict_val});
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

void PropertyLoweringHelper::load_dense_array_cursor(Builder& b, Value* obj, Value*& out_block, Value*& out_head) {
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    out_block = b.build_and(b.build_load(Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET), iconst(b, kPayloadMask));
    out_head = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET));
}

Value* PropertyLoweringHelper::dense_array_slot_addr(Builder& b, Value* block, Value* head, uint32_t index) {
    Value* slot = index == 0 ? head : b.build_add(head, iconst(b, index));
    return b.build_add(b.build_add(block, b.build_shl(slot, iconst(b, 3))), iconst(b, kBronzeHdrBytes));
}

Value* PropertyLoweringHelper::load_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index,
                                                            bool check_hole) {
    Value* elem = nullptr;
    if (index <= kMaxInlineDispIndex) {
        const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(index) * 8);
        elem = b.build_load_indexed(Type::i64(), block, head, 8, disp);
    } else {
        Value* slot = b.build_add(head, iconst(b, index));
        elem = b.build_load_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes));
    }
    if (check_hole) {
        Value* is_hole = b.build_eq(elem, iconst(b, BRONZE_ABI_HOLE_BITS));
        elem = b.build_select(is_hole, iconst(b, kUndefinedTag), elem);
    }
    return elem;
}

void PropertyLoweringHelper::store_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index,
                                                           Value* val) {
    if (index <= kMaxInlineDispIndex) {
        const auto disp = static_cast<int32_t>(kBronzeHdrBytes + static_cast<uint64_t>(index) * 8);
        b.build_store_indexed(Type::i64(), block, head, 8, disp, val);
    } else {
        Value* slot = b.build_add(head, iconst(b, index));
        b.build_store_indexed(Type::i64(), block, slot, 8, static_cast<int32_t>(kBronzeHdrBytes), val);
    }
}

bool PropertyLoweringHelper::is_known_non_ref(Value* val) const {
    if (!val) return false;
    if (boxed_f64_source(val) != nullptr) return true;
    const Instruction* def = val->defining_instruction();
    if (def && def->opcode() == Opcode::bitcast_i64_tagged && def->operand_count() >= 1) {
        const Instruction* inner = def->operand(0)->defining_instruction();
        if (inner && inner->opcode() == Opcode::bitcast_tagged_i64 && inner->operand_count() >= 1) {
            def = inner->operand(0)->defining_instruction();
        }
    }
    if (!def) return false;
    if (def->opcode() == Opcode::iconst_i64) {
        const uint64_t imm = static_cast<uint64_t>(def->imm_i64());
        return imm <= kBronzeNumberMaxBits || imm == kUndefinedTag || imm == kNullTag ||
               imm == kBoolTag || imm == (kBoolTag | 1);
    }
    return def->opcode() == Opcode::bitcast_i64_f64;
}

void PropertyLoweringHelper::emit_dense_array_guard(Builder& b, Value* obj, uint32_t max_index, bool writable,
                                                    Value*& out_ok, Value*& out_block, Value*& out_head) {
    const bool can_select = in_process_ || (b.current_module() && b.current_module()->pinned_tls_register());
    auto emit_header_checks = [&](Value* base, Value* is_obj_val, Value*& ok, Value*& block, Value*& head) {
        Value* hdr_ok = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kArrayHeaderLow));
        Value* is_array = is_obj_val ? b.build_and(is_obj_val, hdr_ok) : hdr_ok;
        Value* elems = b.build_load(Type::i64(), base, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
        Value* has_elems = b.build_eq(b.build_and(elems, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
        block = b.build_and(elems, iconst(b, kPayloadMask));
        head = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_HEAD_OFFSET));
        if (!writable) {
            Value* len = b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
            Value* in_bounds = b.build_ult(b.build_iconst_i32(static_cast<int32_t>(max_index)), len);
            ok = b.build_and(is_array, b.build_and(in_bounds, has_elems));
        } else {
            Value* len = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
            Value* cap = b.build_zext_i64(b.build_load(Type::i32(), base, BRONZE_ABI_ARRAY_CAPACITY_OFFSET));
            Value* max_slot = max_index == 0 ? head : b.build_add(head, iconst(b, max_index));
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
        emit_header_checks(base, in_process_ ? nullptr : is_obj, out_ok, out_block, out_head);
        return;
    }

    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "dense_guard_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    out_ok = b.add_block_param(bb_merge, Type::i32());
    out_block = b.add_block_param(bb_merge, Type::i64());
    out_head = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(cur);
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_pointer(b, obj, ptr, is_obj);
    b.build_br_if(is_obj, bb_arr, {}, bb_merge, {b.build_iconst_i32(0), iconst(b, 0), iconst(b, 0)});

    b.position_at_end(bb_arr);
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_header_checks(ptr, nullptr, ok, block, head);
    b.build_br(bb_merge, {ok, block, head});

    b.position_at_end(bb_merge);
}

Value* PropertyLoweringHelper::lower_is_dense_array(Builder& b, Value* obj, uint32_t max_index) {
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_dense_array_guard(b, obj, max_index, /*writable=*/false, ok, block, head);
    return ok;
}

Value* PropertyLoweringHelper::lower_prop_get_index(Builder& b, Value* obj, uint32_t key_index,
                                                    uint32_t elem_index, const IcSite& site,
                                                    bool check_hole) {
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_dense_array_guard(b, obj, elem_index, /*writable=*/false, ok, block, head);
    BasicBlock* guard_bb = b.current_block();

    Function* fn = guard_bb->parent();
    const std::string prefix = "prop_idx_get_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(guard_bb);
    b.build_br_if(ok, bb_fast, bb_slow);

    b.position_at_end(bb_fast);
    Value* fast_val = load_proven_dense_array_elem(b, block, head, elem_index, check_hole);
    b.build_br(bb_merge, {fast_val});

    b.position_at_end(bb_slow);
    Value* slow_val = lower_prop_get(b, obj, key_index, site ? site_ptr(b, site) : nullptr);
    b.build_br(bb_merge, {slow_val});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::lower_prop_set_index(Builder& b, Value* obj, uint32_t key_index,
                                                  uint32_t elem_index, Value* val, uint32_t strict,
                                                  const IcSite& site, bool val_is_primitive) {
    Value* ok = nullptr;
    Value* block = nullptr;
    Value* head = nullptr;
    emit_dense_array_guard(b, obj, elem_index, /*writable=*/true, ok, block, head);
    BasicBlock* guard_bb = b.current_block();

    const bool need_barrier = !val_is_primitive && !is_known_non_ref(val);
    Function* fn = guard_bb->parent();
    const std::string prefix = "prop_idx_set_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_fast");
    BasicBlock* bb_barrier = need_barrier ? b.append_block(prefix + "_barrier") : nullptr;
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(guard_bb);
    b.build_br_if(ok, bb_fast, bb_slow);

    b.position_at_end(bb_fast);
    store_proven_dense_array_elem(b, block, head, elem_index, val);
    if (need_barrier) {
        Value* is_ref = b.build_ugt(val, iconst(b, kBronzeNumberMaxBits));
        b.build_br_if(is_ref, bb_barrier, bb_merge);
        b.position_at_end(bb_barrier);
        Value* slot_addr = dense_array_slot_addr(b, block, head, elem_index);
        b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
        b.build_br(bb_merge);
    } else {
        b.build_br(bb_merge);
    }

    b.position_at_end(bb_slow);
    lower_prop_set(b, obj, key_index, val, strict, site ? site_ptr(b, site) : nullptr);
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
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
    return box_f64(b, f64_val);
}

void PropertyLoweringHelper::object_payload(Builder& b, Value* obj, Value*& out_ptr, Value*& out_is_obj) {
    object_pointer(b, obj, out_ptr, out_is_obj);
}

Value* PropertyLoweringHelper::load_typed_array_data(Builder& b, Value* ptr) {
    return typed_array_data(b, ptr);
}

} // namespace il2mir
