// Keyed element reads and writes on a receiver PROVEN a word view, an
// Int32Array, Uint32Array, Float32Array or Float64Array:
// `is.typed_array r, kTypedArrayWordView` (il_pin.h) dominates them, and the
// typed-body versioning pass (lower/typed_body_version.h) marked them with
// kElemFlagWordViewReceiver. A view's kind never changes, so what a site
// skips is the generic path's object, Array and view header compares and its
// kind ladder over every kind; it asks only which of the four it is (a store
// which of three: both integer kinds store the same 32 bits). Everything that
// changes with the view — its length (0 once detached, the tracked length of
// a length-tracking view) and where its bytes are — is read at the access,
// before its first branch, so the next access to the same view shares the
// loads.
//
// The access answers exactly what the generic one does. With a Number key,
// an index outside the view reads `undefined` and drops a Number store
// (10.4.5.4 / 10.4.5.5), with no call, so a run of such accesses calls
// nothing; a key that is no Number, or a value that is no Number (ToNumber
// may run user code), takes the generic helper. An integer kind stores
// ToInt32 of the Number, computed inline for every Number.

#include "il/il_pin.h"
#include "il_abi.h"
#include "il_elem_stub.h"
#include "il_lowering_ops.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <string>

namespace il2mir {

namespace {

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;

Value* iconst(Builder& b, uint64_t v) { return b.build_iconst_i64(static_cast<int64_t>(v)); }

Value* view_length(Builder& b, Value* ptr) {
    return b.build_zext_i64(
        with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass));
}

Value* view_kind(Builder& b, Value* ptr) {
    return with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
}

// Float32 and Float64 are the word kinds above the two integer ones.
Value* is_float_kind(Builder& b, Value* kind) {
    return b.build_uge(kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32));
}

// Whether an index operand is a Number by its type or its definition.
bool known_number(Value* index) {
    return index->type() == Type::i32() || index->type() == Type::f64() || boxed_f64_source(index) != nullptr;
}

}  // namespace

bool PropertyLoweringHelper::elem_word_view_receiver(int64_t flags) {
    return (flags & bronze::il::kElemFlagWordViewReceiver) != 0;
}

Value* PropertyLoweringHelper::lower_elem_get_word_view(Builder& b, Value* obj, Value* index, int64_t flags) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_get_wv_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_load = b.append_block(prefix + "_load");
    BasicBlock* bb_float = b.append_block(prefix + "_float");
    BasicBlock* bb_f32 = b.append_block(prefix + "_f32");
    BasicBlock* bb_f64 = b.append_block(prefix + "_f64");
    BasicBlock* bb_int = b.append_block(prefix + "_int");
    BasicBlock* bb_u32 = b.append_block(prefix + "_u32");
    BasicBlock* bb_i32 = b.append_block(prefix + "_i32");
    BasicBlock* bb_box = b.append_block(prefix + "_box");
    BasicBlock* bb_miss = b.append_block(prefix + "_miss");
    bb_miss->set_cold();
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* number = b.add_block_param(bb_box, Type::f64());
    Value* result = b.add_block_param(bb_merge, Type::i64());

    // The view's length, bytes and kind before any branch, as the typed
    // element ops place them: in the block that dominates the access, where
    // the next access to the same view finds them (GVN). No path of an
    // access by a Number index calls anything, so nothing between two
    // accesses clobbers them.
    b.position_at_end(cur);
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    Value* length = view_length(b, ptr);
    Value* data = load_typed_array_data(b, ptr);
    Value* kind = view_kind(b, ptr);
    Value* is_float = is_float_kind(b, kind);
    Value* idx = nullptr;
    Value* ok = nullptr;
    emit_elem_index_guard(b, index, idx, ok, (flags & bronze::il::kElemFlagProvenIntegralIndex) != 0);
    b.build_br_if(b.build_and(ok, b.build_ult(idx, length)), bb_load, bb_miss);

    b.position_at_end(bb_load);
    b.build_br_if(is_float, bb_float, bb_int);
    b.position_at_end(bb_float);
    b.build_br_if(b.build_eq(kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32)), bb_f32, bb_f64);
    b.position_at_end(bb_f32);
    Value* f32 = with_alias_class(b.build_load_indexed(Type::f32(), data, idx, 4), kTypedElementAliasClass);
    b.build_br(bb_box, {b.build_fpext_f64_f32(f32)});
    b.position_at_end(bb_f64);
    b.build_br(bb_box, {with_alias_class(b.build_load_indexed(Type::f64(), data, idx, 8), kTypedElementAliasClass)});
    b.position_at_end(bb_int);
    Value* word = with_alias_class(b.build_load_indexed(Type::i32(), data, idx, 4), kTypedElementAliasClass);
    b.build_br_if(b.build_eq(kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_UINT32)), bb_u32, bb_i32);
    b.position_at_end(bb_u32);
    b.build_br(bb_box, {b.build_sitofp_f64_i64(b.build_zext_i64(word))});
    b.position_at_end(bb_i32);
    b.build_br(bb_box, {b.build_sitofp_f64_i32(word)});
    b.position_at_end(bb_box);
    b.build_br(bb_merge, {box_f64_bits(b, number)});

    // Every Number key of a view is a numeric index (CanonicalNumericIndexString
    // of a Number's string is that Number, "-0" aside, which ToString never
    // makes): one that is no valid integer index reads `undefined` with no
    // lookup (10.4.5.4). Only a key that is no Number takes the helper.
    b.position_at_end(bb_miss);
    Value* undef = iconst(b, kUndefinedTag);
    if (known_number(index)) {
        b.build_br(bb_merge, {undef});
    } else {
        BasicBlock* bb_slow = b.append_block(prefix + "_slow");
        bb_slow->set_cold();
        b.position_at_end(bb_miss);
        b.build_br_if(b.build_ule(index, iconst(b, kBronzeNumberMaxBits)), bb_merge, {undef}, bb_slow, {});
        b.position_at_end(bb_slow);
        b.build_br(bb_merge, {b.build_call(elem_get_helper(), Type::i64(), {obj, boxed_elem_key(b, index)})});
    }

    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::lower_elem_set_word_view(Builder& b, Value* obj, Value* index, Value* val,
                                                      uint32_t strict, int64_t flags) {
    BasicBlock* cur = b.current_block();
    Function* fn = cur->parent();
    const std::string prefix = "elem_set_wv_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_valid = b.append_block(prefix + "_valid");
    bb_valid->set_cold();
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_float = b.append_block(prefix + "_float");
    BasicBlock* bb_f32 = b.append_block(prefix + "_f32");
    BasicBlock* bb_f64 = b.append_block(prefix + "_f64");
    BasicBlock* bb_int = b.append_block(prefix + "_int");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(cur);
    Value* ptr = b.build_and(obj, iconst(b, kPayloadMask));
    Value* length = view_length(b, ptr);
    Value* data = load_typed_array_data(b, ptr);
    Value* kind = view_kind(b, ptr);
    Value* is_float = is_float_kind(b, kind);
    Value* idx = nullptr;
    Value* ok = nullptr;
    emit_elem_index_guard(b, index, idx, ok, (flags & bronze::il::kElemFlagProvenIntegralIndex) != 0);
    // A Number value stores as itself (Float32 rounded, an integer kind its
    // ToInt32); anything else is ToNumber'd by the helper, which may run
    // user code, before the index is looked at.
    Value* num = boxed_f64_source(val);
    Value* num_ok = nullptr;
    if (!num) {
        num_ok = b.build_ule(val, iconst(b, kBronzeNumberMaxBits));
        num = b.build_bitcast_f64_i64(val);
    }
    Value* hit = b.build_and(ok, b.build_ult(idx, length));
    if (num_ok) hit = b.build_and(hit, num_ok);
    b.build_br_if(hit, bb_store, bb_valid);

    // A Number value at a Number key that is no valid integer index: the
    // store is dropped (10.4.5.5, TypedArraySetElement), with nothing to
    // convert first. Anything else, the helper; omitted altogether when both
    // index and value are proven Numbers so no call block enters the CFG.
    b.position_at_end(bb_valid);
    Value* plain = num_ok;
    if (!known_number(index)) {
        Value* key_num = b.build_ule(index, iconst(b, kBronzeNumberMaxBits));
        plain = plain ? b.build_and(plain, key_num) : key_num;
    }
    if (plain) {
        BasicBlock* bb_slow = b.append_block(prefix + "_slow");
        bb_slow->set_cold();
        b.position_at_end(bb_valid);
        b.build_br_if(plain, bb_merge, bb_slow);
        b.position_at_end(bb_slow);
        b.build_call(elem_set_helper(), Type::void_type(),
                     {obj, boxed_elem_key(b, index), val, b.build_iconst_i32(static_cast<int32_t>(strict))});
        b.build_br(bb_merge);
    } else {
        b.build_br(bb_merge);
    }

    b.position_at_end(bb_store);
    b.build_br_if(is_float, bb_float, bb_int);
    b.position_at_end(bb_float);
    b.build_br_if(b.build_eq(kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32)), bb_f32, bb_f64);
    b.position_at_end(bb_f32);
    with_alias_class(b.build_store_indexed(Type::f32(), data, idx, 4, b.build_fptrunc_f32_f64(num)),
                     kTypedElementAliasClass);
    b.build_br(bb_merge);
    b.position_at_end(bb_f64);
    with_alias_class(b.build_store_indexed(Type::f64(), data, idx, 8, num), kTypedElementAliasClass);
    b.build_br(bb_merge);

    // Int32 and Uint32 store the same bits, ToInt32's, computed with no call
    // for every Number, so that a store of a Number calls nothing and the
    // loads above stay shared with the next access.
    b.position_at_end(bb_int);
    Value* word = narrow_f64_to_i32_callfree(b, num);
    with_alias_class(b.build_store_indexed(Type::i32(), data, idx, 4, word), kTypedElementAliasClass);
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

}  // namespace il2mir
