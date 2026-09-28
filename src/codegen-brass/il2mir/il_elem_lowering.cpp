// Keyed element reads and writes (`o[i]`): the inline paths for an Array or a
// TypedArray read at an integral in-bounds index, and the runtime helpers
// (bronze_elem_get / bronze_elem_set) for everything else.
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
// The header word's low half: the object tag, then HeapObjectHeader::flags.
constexpr int32_t kArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr int32_t kTypedArrayHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;

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
// misses there), `boxed` the Value the helper takes.
struct ElemIndex {
    Value* i64 = nullptr;
    Value* ok = nullptr;
    Value* boxed = nullptr;
};

// The f64 an i64 Value was boxed from here (box_f64, IlLowering::ensure_type:
// a select of the canonical NaN and the bits), or null.
Value* boxed_f64_source(Value* v) {
    const Instruction* def = v->defining_instruction();
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
        r.boxed = boxed_index(b, index);
        return r;
    }
    Value* f = nullptr;
    Value* is_number = nullptr;
    if (index->type() == Type::f64()) {
        f = index;
        r.boxed = box_f64(b, index);
    } else if (Value* src = boxed_f64_source(index)) {
        f = src;
        r.boxed = index;
    } else {
        // A Number's bits are the double; any other Value is a tag above them.
        r.boxed = index;
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
    b.build_br(bb_merge, {b.build_call("bronze_elem_get", Type::i64(), {obj, idx.boxed})});

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::lower_elem_set(Builder& b, Value* obj, Value* index, Value* val, uint32_t ic_slot) {
    // Every store takes the helper: an Array element write has to honour
    // frozen and non-writable elements and setters up the chain, and a typed
    // store its kind's conversion.
    Value* ic_val = b.build_iconst_i32(static_cast<int32_t>(ic_slot));
    b.build_call("bronze_elem_set", Type::void_type(), {obj, boxed_index(b, index), val, ic_val});
}

} // namespace il2mir
