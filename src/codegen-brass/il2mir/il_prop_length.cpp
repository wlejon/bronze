// The `length` reads lowered inline: a property read of `length` on any
// receiver, a proven Array's, and the self-hosted builtins'
// LengthOfArrayLike. Each answers from a header where the receiver has one.

#include "il_abi.h"
#include "il_property.h"
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

// `obj.length` read inline for the three receivers whose length is not a slot:
// a String, from its header; an Array, from its header (10.4.2's exotic `length`, which the runtime's
// read answers the same way for every array, `arguments` included), and a
// typed array, from its view header — but only when the runtime's own read
// would answer from it (rtTypedArrayChainPristine): the view's shape is its
// kind's intrinsic instance shape (no own property, its own prototype) and
// the family was verified pristine at the current prototype-mutation epoch
// (TLS ta_length_tbl). The header's length IS the getter's answer, 0 for a
// detached or out-of-bounds view (typed_array.h keeps the truth in the
// field). Any other object takes the site's way-0 guard when `mono`, and
// everything else the helper.
Value* PropertyLoweringHelper::lower_prop_get_length(Builder& b, Value* obj, uint32_t key_index, const IcSite& site,
                                                     bool mono, bool is_family) {
    Value* ptr = nullptr;
    Value* is_obj = nullptr;
    object_payload(b, obj, ptr, is_obj);
    BasicBlock* cur = b.current_block();
    const std::string prefix = "len_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_not_obj = b.append_block(prefix + "_not_obj");
    BasicBlock* bb_str = b.append_block(prefix + "_str");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_not_arr = b.append_block(prefix + "_not_arr");
    BasicBlock* bb_ta = b.append_block(prefix + "_ta");
    BasicBlock* bb_ta_hit = b.append_block(prefix + "_ta_hit");
    BasicBlock* bb_other = b.append_block(prefix + "_other");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());
    // A loop bound's `length` is a header's: the helper is the exception.
    bb_slow->set_cold();

    b.position_at_end(cur);
    b.build_br_if(is_obj, bb_obj, bb_not_obj);

    // A String's `length` is its code-unit count, which nothing can redefine
    // (22.1.4.1: an own, non-writable, non-configurable property of every
    // String exotic object), and the header holds it for every string,
    // a `+`-chain builder included (string.h).
    b.position_at_end(bb_not_obj);
    Value* tag = b.build_lshr(obj, b.build_iconst_i64(48));
    b.build_br_if(b.build_eq(tag, b.build_iconst_i64(BRONZE_ABI_TAG_STRING)), bb_str, bb_slow);

    b.position_at_end(bb_str);
    Value* str = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(0x0000FFFFFFFFFFFFULL)));
    Value* str_len = b.build_zext_i64(b.build_load(Type::i32(), str, BRONZE_ABI_STRING_LENGTH_OFFSET));
    b.build_br(bb_merge, {box_number(b, b.build_sitofp_f64_i64(str_len))});

    b.position_at_end(bb_obj);
    constexpr int32_t kViewHeaderLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    Value* header_low = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kArrayHeaderLow)), bb_arr, bb_not_arr);

    b.position_at_end(bb_arr);
    Value* arr_len = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    b.build_br(bb_merge, {box_number(b, b.build_sitofp_f64_i64(arr_len))});

    b.position_at_end(bb_not_arr);
    b.build_br_if(b.build_eq(header_low, b.build_iconst_i32(kViewHeaderLow)), bb_ta, bb_other);

    b.position_at_end(bb_ta);
    Value* kind = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET));
    b.build_br_if(typed_array_chain_pristine(b, ptr, kind), bb_ta_hit, bb_slow);

    b.position_at_end(bb_ta_hit);
    Value* ta_len = b.build_zext_i64(with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass));
    b.build_br(bb_merge, {box_number(b, b.build_sitofp_f64_i64(ta_len))});

    // A plain object's `length` is an ordinary property: the site's way 0
    // when the IL calls it monomorphic, the helper otherwise.
    b.position_at_end(bb_other);
    if (mono) {
        b.build_br(bb_merge, {lower_prop_get_mono(b, obj, key_index, site, is_family)});
    } else {
        b.build_br(bb_slow);
    }

    b.position_at_end(bb_slow);
    b.build_br(bb_merge, {lower_prop_get(b, obj, key_index, site)});

    b.position_at_end(bb_merge);
    return result;
}

// `prop.get r, "length"` on a receiver a dominating `is.array` tested (il_pin.h
// kPropGetArrayLength): the header's length, boxed so that a region's
// `is.number` and raw unbox of it fold to the double (boxed_f64_source).
Value* PropertyLoweringHelper::lower_array_length(Builder& b, Value* obj) {
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    return box_number(b, b.build_sitofp_f64_i64(length));
}

// 7.3.18 LengthOfArrayLike(O), the self-hosted builtins' `$LengthOfArrayLike`
// (bronze_sh_length_of_array_like): an Array's header length inline, anything
// else the helper. Both answer a Number (the helper's is ToLength's, never
// NaN), so the two meet as a double and the result is its box, which the
// builtin's `+` unboxes back to the double (boxed_f64_source): its loop bound
// is the header's length, a value brass can see is an integer.
Value* PropertyLoweringHelper::lower_length_of_array_like(Builder& b, Value* obj) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "len_al_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_array = b.append_block(prefix + "_array");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::f64());
    bb_slow->set_cold();

    b.position_at_end(cur);
    Value* is_obj = b.build_eq(b.build_and(obj, iconst(b, kTagMask)), iconst(b, kObjectTagBits));
    b.build_br_if(is_obj, bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow)), bb_array,
                  bb_slow);

    b.position_at_end(bb_array);
    Value* length = b.build_zext_i64(b.build_load(Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET));
    b.build_br(bb_merge, {b.build_sitofp_f64_i64(length)});

    b.position_at_end(bb_slow);
    Value* asked = b.build_call("bronze_sh_length_of_array_like", Type::i64(), {obj});
    b.build_br(bb_merge, {b.build_bitcast_f64_i64(asked)});

    b.position_at_end(bb_merge);
    return box_number(b, result);
}

}  // namespace il2mir
