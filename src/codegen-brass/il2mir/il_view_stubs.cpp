// The module's typed-array element stubs (il_prop_stubs.h kViewGetStub /
// kViewSetStub): what an element read or write of a typed array does once the
// inline path has seen a typed-array header and an int32 index. One copy per
// module rather than at every site, because the element kind is a run-time
// fact of the view and its dispatch is nine arms. Anything they do not answer
// — an index past the length, a Float16 or BigInt view, a value that is no
// Number, a double out of an integer kind's conversion range — is the
// helper's, with the access's own operands.
//
// The layouts are the runtime's (runtime/typed_array.h): the view's buffer
// Value, its byte offset and length (kept current by the runtime on a detach
// or a resize, buffer_views.cpp), its kind; the buffer's external store or
// its inline bytes.
#include "il_prop_stubs.h"

#include "il_abi.h"
#include "il_lowering_ops.h"

#include <brass/mir/block.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/instruction.hpp>
#include <brass/mir/module.hpp>

#include <string>

namespace il2mir {

namespace {

constexpr int64_t kPayloadMask = 0x0000FFFFFFFFFFFFLL;

Value* imm(Builder& b, int64_t v) { return b.build_iconst_i64(v); }

// One arm per element kind off `kind`: `arm(kind)` builds the arm's body in
// its own block; a kind with no arm goes to `other`.
template <typename Arm>
void kind_switch(Builder& b, Value* kind, const int (&kinds)[9], BasicBlock* other, const std::string& prefix,
                 const Arm& arm) {
    for (int k : kinds) {
        BasicBlock* cur = b.current_block();
        BasicBlock* body = b.append_block(prefix + "_k" + std::to_string(k));
        BasicBlock* next = b.append_block(prefix + "_n" + std::to_string(k));
        b.position_at_end(cur);
        b.build_br_if(b.build_eq(kind, b.build_iconst_i32(k)), body, next);
        b.position_at_end(body);
        arm(k);
        b.position_at_end(next);
    }
    b.build_br(other);
}

// The kinds in the order a program meets them most: Float32 (geometry),
// Float64, then the integer views.
constexpr int kKinds[9] = {BRONZE_ABI_TA_KIND_FLOAT32, BRONZE_ABI_TA_KIND_FLOAT64, BRONZE_ABI_TA_KIND_UINT8,
                           BRONZE_ABI_TA_KIND_INT32,   BRONZE_ABI_TA_KIND_UINT32,  BRONZE_ABI_TA_KIND_UINT16,
                           BRONZE_ABI_TA_KIND_INT16,   BRONZE_ABI_TA_KIND_INT8,    BRONZE_ABI_TA_KIND_UINT8CLAMPED};

Value* typed(Builder& /*b*/, Value* v) { return with_alias_class(v, kTypedElementAliasClass); }

// The low `bits` of `v` (an i64 holding them zero-extended), sign-extended.
Value* sign_extend(Builder& b, Value* v, int bits) {
    return b.build_ashr(b.build_shl(v, imm(b, 64 - bits)), imm(b, 64 - bits));
}

// A 16-bit element zero-extended: MIR widens an i8 but not an i16, so it is
// read as its two bytes (little-endian, as every target brass runs on is).
Value* load_u16(Builder& b, Value* data, Value* idx) {
    Value* lo = b.build_zext_i64(typed(b, b.build_load_indexed(Type::i8(), data, idx, 2, 0)));
    Value* hi = b.build_zext_i64(typed(b, b.build_load_indexed(Type::i8(), data, idx, 2, 1)));
    return b.build_or(lo, b.build_shl(hi, imm(b, 8)));
}

}  // namespace

// Header loads on a typed array or its backing buffer after an already
// proven non-null typed-array check never fault and may be hoisted above a
// typed-length bounds check once `non_trapping_load` is set.
Value* non_trapping_header(Value* v) {
    Value* tagged = with_alias_class(v, kTypedArrayHeaderAliasClass);
    if (auto* inst = tagged->defining_instruction()) inst->set_non_trapping_load(true);
    return tagged;
}

// The buffer's external store, or the bytes inline after its header, plus
// byteOffset.
Value* view_data(Builder& b, Value* ptr) {
    Value* buf = b.build_and(non_trapping_header(b.build_load(Type::i64(), ptr, BRONZE_ABI_TA_BUFFER_OFFSET)),
                             imm(b, kPayloadMask));
    Value* ext = non_trapping_header(b.build_load(Type::i64(), buf, BRONZE_ABI_BUF_EXTPTR_OFFSET));
    Value* data = b.build_select(b.build_ne(ext, imm(b, 0)), ext, b.build_add(buf, imm(b, BRONZE_ABI_BUF_DATA_OFFSET)));
    Value* byte_offset =
        b.build_zext_i64(non_trapping_header(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET)));
    return b.build_add(data, byte_offset);
}

Value* view_element_load(Builder& b, Value* data, Value* idx, int kind) {
    switch (kind) {
        case BRONZE_ABI_TA_KIND_FLOAT64:
            return typed(b, b.build_load_indexed(Type::f64(), data, idx, 8));
        case BRONZE_ABI_TA_KIND_FLOAT32:
            return b.build_fpext_f64_f32(typed(b, b.build_load_indexed(Type::f32(), data, idx, 4)));
        case BRONZE_ABI_TA_KIND_INT32:
            return typed(b, b.build_load_indexed(Type::i32(), data, idx, 4));
        case BRONZE_ABI_TA_KIND_UINT32:
            return b.build_sitofp_f64_i64(b.build_zext_i64(typed(b, b.build_load_indexed(Type::i32(), data, idx, 4))));
        case BRONZE_ABI_TA_KIND_UINT16:
            return b.build_trunc_i32(load_u16(b, data, idx));
        case BRONZE_ABI_TA_KIND_INT16:
            return b.build_trunc_i32(sign_extend(b, load_u16(b, data, idx), 16));
        case BRONZE_ABI_TA_KIND_INT8:
            return b.build_trunc_i32(
                sign_extend(b, b.build_zext_i64(typed(b, b.build_load_indexed(Type::i8(), data, idx, 1))), 8));
        default:  // UINT8, UINT8CLAMPED
            return b.build_trunc_i32(b.build_zext_i64(typed(b, b.build_load_indexed(Type::i8(), data, idx, 1))));
    }
}

void view_element_store(Builder& b, Value* data, Value* idx, int kind, Value* v) {
    switch (kind) {
        case BRONZE_ABI_TA_KIND_FLOAT64:
            with_alias_class(b.build_store_indexed(Type::f64(), data, idx, 8, v), kTypedElementAliasClass);
            return;
        case BRONZE_ABI_TA_KIND_FLOAT32:
            with_alias_class(b.build_store_indexed(Type::f32(), data, idx, 4, b.build_fptrunc_f32_f64(v)),
                             kTypedElementAliasClass);
            return;
        case BRONZE_ABI_TA_KIND_INT32:
        case BRONZE_ABI_TA_KIND_UINT32:
            with_alias_class(b.build_store_indexed(Type::i32(), data, idx, 4, v), kTypedElementAliasClass);
            return;
        case BRONZE_ABI_TA_KIND_INT16:
        case BRONZE_ABI_TA_KIND_UINT16: {
            Value* n = b.build_zext_i64(v);
            with_alias_class(b.build_store_indexed(Type::i8(), data, idx, 2, 0, b.build_trunc_i8(n)),
                             kTypedElementAliasClass);
            with_alias_class(
                b.build_store_indexed(Type::i8(), data, idx, 2, 1, b.build_trunc_i8(b.build_lshr(n, imm(b, 8)))),
                kTypedElementAliasClass);
            return;
        }
        case BRONZE_ABI_TA_KIND_UINT8CLAMPED: {
            // ToUint8Clamp (7.1.12): NaN and anything not above 0 is 0, from
            // 255 up 255, else the nearest integer, a tie to the even one.
            Value* above0 = b.build_slt(b.build_fconst_f64(0.0), v);
            Value* c = b.build_select(b.build_slt(v, b.build_fconst_f64(255.0)), v, b.build_fconst_f64(255.0));
            Value* f = b.build_floor_f64(c);
            Value* d = b.build_sub(c, f);
            Value* fi = b.build_fptosi_i32(f);
            Value* odd = b.build_ne(b.build_and(fi, b.build_iconst_i32(1)), b.build_iconst_i32(0));
            Value* half = b.build_fconst_f64(0.5);
            Value* up = b.build_or(b.build_slt(half, d), b.build_and(b.build_eq(d, half), odd));
            Value* r = b.build_add(fi, b.build_select(up, b.build_iconst_i32(1), b.build_iconst_i32(0)));
            r = b.build_select(above0, r, b.build_iconst_i32(0));
            with_alias_class(b.build_store_indexed(Type::i8(), data, idx, 1, b.build_trunc_i8(b.build_zext_i64(r))),
                             kTypedElementAliasClass);
            return;
        }
        default:  // INT8, UINT8
            with_alias_class(b.build_store_indexed(Type::i8(), data, idx, 1, b.build_trunc_i8(b.build_zext_i64(v))),
                             kTypedElementAliasClass);
            return;
    }
}

// (obj, idx, key) -> the element as a Number.
void build_view_get_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* idx32 = b.add_block_param(entry, Type::i32());
    Value* key = b.add_block_param(entry, Type::i64());
    BasicBlock* in = b.append_block("vg_in");
    BasicBlock* helper = b.append_block("vg_helper");

    b.position_at_end(entry);
    Value* ptr = object_address(b, obj);
    Value* len = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass);
    b.build_br_if(b.build_ult(idx32, len), in, helper);

    b.position_at_end(in);
    Value* kind = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
    Value* data = view_data(b, ptr);
    Value* idx = b.build_zext_i64(idx32);
    kind_switch(b, kind, kKinds, helper, "vg", [&](int k) {
        Value* v = view_element_load(b, data, idx, k);
        if (v->type() == Type::i32()) v = b.build_sitofp_f64_i32(v);
        b.build_ret(box_f64_bits(b, v));
    });

    b.position_at_end(helper);
    b.build_ret(b.build_call("bronze_elem_get", Type::i64(), {obj, key}));
    fn->rebuild_cfg_predecessors();
}

// (obj, idx, key, val, strict) -> void: a Number stored as the kind
// converts it (ToInt32-family modular truncation for the integer kinds,
// rounding to float for Float32, ToUint8Clamp for Uint8Clamped).
void build_view_set_stub(Function* fn) {
    Builder b(*fn->parent());
    b.set_function(fn);
    BasicBlock* entry = b.append_block("entry");
    Value* obj = b.add_block_param(entry, Type::i64());
    Value* idx32 = b.add_block_param(entry, Type::i32());
    Value* key = b.add_block_param(entry, Type::i64());
    Value* val = b.add_block_param(entry, Type::i64());
    Value* strict = b.add_block_param(entry, Type::i32());
    BasicBlock* in = b.append_block("vs_in");
    BasicBlock* helper = b.append_block("vs_helper");

    b.position_at_end(entry);
    Value* ptr = object_address(b, obj);
    Value* len = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass);
    Value* number = b.build_ule(val, imm(b, static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(b.build_and(number, b.build_ult(idx32, len)), in, helper);

    b.position_at_end(in);
    Value* kind = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
    Value* data = view_data(b, ptr);
    Value* idx = b.build_zext_i64(idx32);
    Value* f = b.build_bitcast_f64_i64(val);
    // An integer kind's value: the double truncated toward zero, modulo
    // 2^bits, which an i64 conversion's low bits are for every double below
    // 2^63 in magnitude. NaN, the infinities and the rest are the helper's
    // (their ToInt32 is 0 or a reduction an i64 cannot hold).
    Value* in_range = b.build_ult(b.build_and(val, imm(b, 0x7FFFFFFFFFFFFFFFLL)), imm(b, 0x43E0000000000000LL));
    kind_switch(b, kind, kKinds, helper, "vs", [&](int k) {
        if (k == BRONZE_ABI_TA_KIND_FLOAT64 || k == BRONZE_ABI_TA_KIND_FLOAT32 || k == BRONZE_ABI_TA_KIND_UINT8CLAMPED) {
            view_element_store(b, data, idx, k, f);
            b.build_ret_void();
            return;
        }
        BasicBlock* cur = b.current_block();
        BasicBlock* conv = b.append_block("vs_conv" + std::to_string(k));
        b.position_at_end(cur);
        b.build_br_if(in_range, conv, helper);
        b.position_at_end(conv);
        view_element_store(b, data, idx, k, b.build_trunc_i32(b.build_fptosi_i64(f)));
        b.build_ret_void();
    });

    b.position_at_end(helper);
    b.build_call("bronze_elem_set", Type::void_type(), {obj, key, val, strict});
    b.build_ret_void();
    fn->rebuild_cfg_predecessors();
}

}  // namespace il2mir
