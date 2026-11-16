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
#include "il_prop_stubs.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <algorithm>
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

// The typed-array arm of an element access. A build made to be harvested
// counts kElemArmView and, for each set bit of the view kind's code (il_abi.h
// kElemViewKinds), that bit's arm; a kind with no code and an index at or past
// the view's length mark kElemArmMiss. Any other build marks kElemArmView.
void mark_view_arm(PropertyLoweringHelper& prop, Builder& b, Value* ptr, Value* idx, const std::string& prefix) {
    if (!prop.evidence_marked()) {
        prop.arm_begin(b, kElemArmView);
        return;
    }
    // Per bit, the kinds whose code has it set; and the kinds with a code.
    uint32_t bitKinds[3] = {0, 0, 0};
    uint32_t coded = 0;
    for (uint32_t code = 0; code < 8; ++code) {
        coded |= 1u << kElemViewKinds[code];
        for (uint32_t bit = 0; bit < 3; ++bit) {
            if ((code >> bit) & 1u) bitKinds[bit] |= 1u << kElemViewKinds[code];
        }
    }
    const uint32_t bitArms[3] = {kElemArmKindBit0, kElemArmKindBit1, kElemArmKindBit2};
    // append_block moves the cursor, so every block is made first.
    BasicBlock* at = b.current_block();
    BasicBlock* known = b.append_block(prefix + "_vknown");
    BasicBlock* sets[3];
    BasicBlock* afters[3];
    for (int i = 0; i < 3; ++i) {
        sets[i] = b.append_block(prefix + "_vbit" + std::to_string(i));
        afters[i] = b.append_block(prefix + "_vbitn" + std::to_string(i));
    }
    BasicBlock* miss = b.append_block(prefix + "_vmiss");
    BasicBlock* bounds = b.append_block(prefix + "_vbounds");
    BasicBlock* in = b.append_block(prefix + "_vin");

    b.position_at_end(at);
    Value* kind = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
    Value* vlen = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass);
    // The kind's bit in a mask of kinds: below 32, so the shift is defined.
    Value* kindBit = b.build_shl(b.build_iconst_i32(1), kind);
    const auto in_mask = [&](uint32_t mask) {
        return b.build_ne(b.build_and(kindBit, b.build_iconst_i32(static_cast<int32_t>(mask))), b.build_iconst_i32(0));
    };
    b.build_br_if(in_mask(coded), known, miss);
    b.position_at_end(known);
    prop.arm_count(b, kElemArmView);
    for (int i = 0; i < 3; ++i) {
        b.build_br_if(in_mask(bitKinds[i]), sets[i], afters[i]);
        b.position_at_end(sets[i]);
        prop.arm_count(b, bitArms[i]);
        b.build_br(afters[i]);
        b.position_at_end(afters[i]);
    }
    b.build_br(bounds);
    b.position_at_end(bounds);
    b.build_br_if(b.build_ult(idx, vlen), in, miss);
    b.position_at_end(miss);
    prop.arm_begin(b, kElemArmMiss);
    b.build_br(in);
    b.position_at_end(in);
}

}  // namespace

Value* PropertyLoweringHelper::elem_cell(Builder& b) {
    if (elem_cell_sites_ == nullptr || !elem_cells_base_fn_ || slow_sites_.size() != 1) return nullptr;
    const auto it = std::lower_bound(elem_cell_sites_->begin(), elem_cell_sites_->end(), slow_sites_[0]);
    if (it == elem_cell_sites_->end() || *it != slow_sites_[0]) return nullptr;
    const int64_t at = static_cast<int64_t>(it - elem_cell_sites_->begin()) * kElemCellBytes;
    return b.build_add(elem_cells_base_fn_(b), iconst(b, static_cast<uint64_t>(at)));
}

// An element key as the Value the helpers take: an i32 is an int32 Number, an
// f64 its boxed bits, anything else already a Value.
Value* boxed_elem_key(Builder& b, Value* index) {
    if (index->type() == Type::i32()) return b.build_bitcast_i64_f64(b.build_sitofp_f64_i32(index));
    if (index->type() == Type::f64()) return box_f64_bits(b, index);
    return index;
}

// An element key's index as the inline paths take it: an i32 as it is; a
// Number that is an int32 (a double whose truncation converts back to it,
// which NaN, an infinity, a fraction and anything out of range never do;
// -0 is index 0, which ToPropertyKey says it is). `ok` is null when the key
// is an i32 already.
struct ElemIndex {
    Value* idx = nullptr;
    Value* ok = nullptr;
};

ElemIndex elem_index(Builder& b, Value* index) {
    if (index->type() == Type::i32()) return {index, nullptr};
    if (index->type() == Type::f64()) {
        Value* k = b.build_fptosi_i32(index);
        return {k, b.build_eq(b.build_sitofp_f64_i32(k), index)};
    }
    Value* bits = index->type().is_tagged() ? b.build_bitcast_tagged_i64(index) : index;
    Value* f = b.build_bitcast_f64_i64(bits);
    Value* k = b.build_fptosi_i32(f);
    Value* number = b.build_ule(bits, iconst(b, kBronzeNumberMaxBits));
    return {k, b.build_and(number, b.build_eq(b.build_sitofp_f64_i32(k), f))};
}

// The ARMS of an element access (il_abi.h kElemArmArray / kElemArmView),
// marked in a build that may be harvested where the access takes them: the
// Array arm is exactly the speculation step's element guards (an Array, no
// index hazard, an int32 index in bounds, a present element; a write also
// at the bare array shape), the View arm a typed array, and everything
// else — an append included — the slow arm (opt/speculate/evidence.cpp
// planElem).
//
// A read: an Array's element inline, a typed array's through the module's
// view stub, a plain receiver's through the site's pair cell when it names
// the receiver's shape and the key, the rest bronze_elem_get.
Value* PropertyLoweringHelper::lower_elem_get(Builder& b, Value* obj, Value* index) {
    Value* key = boxed_elem_key(b, index);
    const ElemIndex ei = elem_index(b, index);
    BasicBlock* cur = b.current_block();
    const std::string prefix = "elem_get_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_load = b.append_block(prefix + "_load");
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_not_arr = b.append_block(prefix + "_notarr");
    BasicBlock* bb_view = b.append_block(prefix + "_view");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());
    bb_slow->set_cold();

    b.position_at_end(cur);
    Value* ptr = object_address(b, obj);
    Value* is_obj = is_object_address(b, ptr);
    b.build_br_if(ei.ok ? b.build_and(is_obj, ei.ok) : is_obj, bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* header = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kArrayHeaderLow)), bb_arr, bb_not_arr);

    b.position_at_end(bb_arr);
    Value* elems = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    Value* has_elems = b.build_eq(b.build_and(elems, tls_block_word(b, BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF)),
                                  iconst(b, kObjectTagBits));
    Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    // An Array whose element is not there (past the length, a hole, no
    // elements block) marks kElemArmMiss on its way to the helper.
    BasicBlock* bb_miss = bb_slow;
    if (arms_marked()) {
        bb_miss = b.append_block(prefix + "_miss");
        bb_miss->set_cold();
        b.position_at_end(bb_miss);
        arm_begin(b, kElemArmMiss);
        b.build_br(bb_slow);
        b.position_at_end(bb_arr);
    }
    b.build_br_if(b.build_and(has_elems, b.build_ult(ei.idx, len)), bb_load, bb_miss);

    b.position_at_end(bb_load);
    Value* elem = array_element_load(b, array_elements(b, ptr), b.build_zext_i64(ei.idx));
    b.build_br_if(b.build_eq(elem, iconst(b, BRONZE_ABI_HOLE_BITS)), bb_miss, {}, bb_hit, {});
    b.position_at_end(bb_hit);
    arm_begin(b, kElemArmArray);
    b.build_br(bb_merge, {elem});

    b.position_at_end(bb_not_arr);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kTypedArrayHeaderLow)), bb_view, bb_slow);
    // A Float32 element in bounds is read here (geometry's kind, and the one
    // a call per element costs most against); every other kind and every
    // index past the length is the module's stub.
    b.position_at_end(bb_view);
    mark_view_arm(*this, b, ptr, ei.idx, prefix);
    {
        BasicBlock* at = b.current_block();
        BasicBlock* f32 = b.append_block(prefix + "_f32");
        BasicBlock* stub = b.append_block(prefix + "_vstub");
        b.position_at_end(at);
        Value* kind = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
        Value* vlen = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass);
        b.build_br_if(b.build_and(b.build_eq(kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32)), b.build_ult(ei.idx, vlen)),
                      f32, stub);
        b.position_at_end(f32);
        Value* f = with_alias_class(b.build_load_indexed(Type::f32(), view_data(b, ptr), b.build_zext_i64(ei.idx), 4),
                                    kTypedElementAliasClass);
        b.build_br(bb_merge, {box_f64_bits(b, b.build_fpext_f64_f32(f))});
        b.position_at_end(stub);
        b.build_br(bb_merge, {b.build_call(stub_sym(kViewGetStub), Type::i64(), {obj, ei.idx, key})});
    }

    b.position_at_end(bb_slow);
    slow_arm(b);
    if (Value* cell = elem_cell(b)) {
        // The site's cell (elem_cells.h): a plain receiver at the cell's
        // shape and the cell's key bits, whose slot word names an own slot,
        // reads it here; the module's cell stub answers the cell's other
        // forms and takes everything else to the runtime.
        // append_block moves the cursor, so every block is made first.
        bb_slow->set_cold(false);
        BasicBlock* bb_cell_hit = b.append_block(prefix + "_cellhit");
        BasicBlock* bb_own = b.append_block(prefix + "_cellown");
        BasicBlock* bb_other = b.append_block(prefix + "_cellother");
        BasicBlock* bb_stub = b.append_block(prefix + "_cellstub");
        BasicBlock* bb_generic = b.append_block(prefix + "_cellgeneric");
        b.position_at_end(bb_slow);
        Value* is_object = nullptr;
        Value* cptr = nullptr;
        Value* shape = nullptr;
        receiver_shape(b, obj, is_object, cptr, shape);
        Value* same_shape = b.build_eq(shape, b.build_load(Type::i64(), cell, 0));
        Value* same_key = b.build_eq(key, b.build_load(Type::i64(), cell, 8));
        b.build_br_if(b.build_and(is_object, b.build_and(same_shape, same_key)), bb_cell_hit, bb_other);
        // Another pair: the stub while the site still takes fills, else the
        // generic read straight away (a site that reads several keys in turn
        // misses its cell at most reads for good).
        b.position_at_end(bb_other);
        b.build_br_if(b.build_ult(b.build_load(Type::i64(), cell, 24), iconst(b, BRONZE_ABI_ELEM_CELL_LIMIT)), bb_stub,
                      bb_generic);
        b.position_at_end(bb_generic);
        b.build_br(bb_merge, {b.build_call("bronze_elem_get", Type::i64(), {obj, key})});
        b.position_at_end(bb_cell_hit);
        Value* slot_word = b.build_load(Type::i64(), cell, 16);
        b.build_br_if(b.build_ult(slot_word, iconst(b, uint64_t{1} << 32)), bb_own, bb_stub);
        b.position_at_end(bb_own);
        b.build_br(bb_merge, {load_slot_at(b, cptr, slot_word)});
        b.position_at_end(bb_stub);
        b.build_br(bb_merge, {b.build_call(stub_sym(kElemCellStub), Type::i64(), {obj, key, cell})});
        b.position_at_end(bb_merge);
        return result;
    }
    b.build_br(bb_merge, {b.build_call("bronze_elem_get", Type::i64(), {obj, key})});

    b.position_at_end(bb_merge);
    return result;
}

// A write: an Array's present element stored inline, or one appended at
// its length within its capacity, at the bare array shape (no named property,
// no integrity level) with no index hazard; a typed array's through the
// module's view stub; the rest bronze_elem_set. A value that is no Number
// clears the array's NUMBERS bit (bronze_abi_layout.h) and gets the barrier.
void PropertyLoweringHelper::lower_elem_set(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict) {
    Value* key = boxed_elem_key(b, index);
    const ElemIndex ei = elem_index(b, index);
    if (val->type().is_tagged()) val = b.build_bitcast_tagged_i64(val);
    BasicBlock* cur = b.current_block();
    const std::string prefix = "elem_set_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_present = b.append_block(prefix + "_present");
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_append_test = b.append_block(prefix + "_append_test");
    BasicBlock* bb_append = b.append_block(prefix + "_append");
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    Value* store_at = b.add_block_param(bb_store, Type::i64());
    BasicBlock* bb_not_arr = b.append_block(prefix + "_notarr");
    BasicBlock* bb_view = b.append_block(prefix + "_view");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_done = b.append_block(prefix + "_done");
    bb_slow->set_cold();

    b.position_at_end(cur);
    Value* ptr = object_address(b, obj);
    Value* is_obj = is_object_address(b, ptr);
    b.build_br_if(ei.ok ? b.build_and(is_obj, ei.ok) : is_obj, bb_obj, bb_slow);

    b.position_at_end(bb_obj);
    Value* header = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kArrayHeaderLow)), bb_arr, bb_not_arr);

    b.position_at_end(bb_arr);
    Value* bare = b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                             tls_block_word(b, BRONZE_TLS_ARRAY_SHAPE_OFF));
    Value* elems = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    Value* has_elems = b.build_eq(b.build_and(elems, tls_block_word(b, BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF)),
                                  iconst(b, kObjectTagBits));
    Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    b.build_br_if(b.build_and(bare, has_elems), bb_present, bb_slow);

    b.position_at_end(bb_present);
    const ArrayElements e = array_elements(b, ptr);
    Value* at = b.build_add(b.build_add(e.block, b.build_shl(b.build_add(b.build_zext_i64(ei.idx), e.head),
                                                            iconst(b, 3))),
                            iconst(b, kBronzeHdrBytes));
    BasicBlock* bb_in = b.append_block(prefix + "_in");
    b.position_at_end(bb_present);
    b.build_br_if(b.build_ult(ei.idx, len), bb_in, bb_append_test);
    b.position_at_end(bb_in);
    Value* old = with_alias_class(b.build_load(Type::i64(), at, 0), kBoxedElementAliasClass);
    b.build_br_if(b.build_eq(old, iconst(b, BRONZE_ABI_HOLE_BITS)), bb_slow, {}, bb_hit, {});
    b.position_at_end(bb_hit);
    arm_begin(b, kElemArmArray);
    b.build_br(bb_store, {at});

    // An append: the index is the length, below the capacity (the elements
    // block is as long as the capacity, from the head).
    b.position_at_end(bb_append_test);
    Value* cap = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    Value* room = b.build_ult(b.build_add(e.head, b.build_zext_i64(ei.idx)), b.build_zext_i64(cap));
    b.build_br_if(b.build_and(b.build_eq(ei.idx, len), room), bb_append, bb_slow);
    b.position_at_end(bb_append);
    arm_begin(b, kElemArmGrow);
    array_header_store(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, b.build_add(ei.idx, b.build_iconst_i32(1)));
    b.build_br(bb_store, {at});

    b.position_at_end(bb_store);
    const auto forget_numbers = [ptr](Builder& bb) {
        Value* word = array_header_load(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
        array_header_store(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                           bb.build_and(word, bb.build_iconst_i32(~int32_t{BRONZE_ABI_ARRAY_BIT_NUMBERS})));
    };
    store_value(b, store_at, val, kBoxedElementAliasClass, !is_known_non_ref(val), forget_numbers);
    b.build_br(bb_done);

    b.position_at_end(bb_not_arr);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kTypedArrayHeaderLow)), bb_view, bb_slow);
    // A Number stored into a Float32 element in bounds is written here (as
    // the read is, lower_elem_get); everything else is the module's stub.
    b.position_at_end(bb_view);
    mark_view_arm(*this, b, ptr, ei.idx, prefix);
    {
        BasicBlock* at = b.current_block();
        BasicBlock* f32 = b.append_block(prefix + "_f32");
        BasicBlock* stub = b.append_block(prefix + "_vstub");
        b.position_at_end(at);
        Value* kind = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
        Value* vlen = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET), kTypedArrayHeaderAliasClass);
        Value* number = b.build_ule(val, iconst(b, static_cast<int64_t>(kBronzeNumberMaxBits)));
        b.build_br_if(b.build_and(b.build_and(b.build_eq(kind, b.build_iconst_i32(BRONZE_ABI_TA_KIND_FLOAT32)),
                                              b.build_ult(ei.idx, vlen)),
                                  number),
                      f32, stub);
        b.position_at_end(f32);
        with_alias_class(b.build_store_indexed(Type::f32(), view_data(b, ptr), b.build_zext_i64(ei.idx), 4,
                                               b.build_fptrunc_f32_f64(b.build_bitcast_f64_i64(val))),
                         kTypedElementAliasClass);
        b.build_br(bb_done);
        b.position_at_end(stub);
        b.build_call(stub_sym(kViewSetStub), Type::void_type(),
                     {obj, ei.idx, key, val, b.build_iconst_i32(static_cast<int32_t>(strict))});
        b.build_br(bb_done);
    }

    b.position_at_end(bb_slow);
    slow_arm(b);
    b.build_call("bronze_elem_set", Type::void_type(),
                 {obj, key, val, b.build_iconst_i32(static_cast<int32_t>(strict))});
    b.build_br(bb_done);

    b.position_at_end(bb_done);
}

// `a.push(v)`, one argument, on an Array (`ptr`, its header already tested)
// at the bare array shape: no own property, so `push` is %Array.prototype%'s,
// which lockdown froze. With an elements block, no index hazard (the elements
// tag mask, as the element store's append arm reads it) and room past the
// length, 23.1.3.23's one Set lands in the block: the value at the length,
// the length one more, NUMBERS cleared by a value that is no Number, and the
// barrier. The result is the new length. Anything else goes to `miss`.
Value* PropertyLoweringHelper::lower_array_push(Builder& b, Value* ptr, Value* val, BasicBlock* miss) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "apush_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* room_test = b.append_block(prefix + "_room");
    BasicBlock* store = b.append_block(prefix + "_store");
    b.position_at_end(cur);
    Value* bare = b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                             tls_block_word(b, BRONZE_TLS_ARRAY_SHAPE_OFF));
    Value* elems = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    Value* has_elems = b.build_eq(b.build_and(elems, tls_block_word(b, BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF)),
                                  iconst(b, kObjectTagBits));
    b.build_br_if(b.build_and(bare, has_elems), room_test, miss);
    b.position_at_end(room_test);
    Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    Value* cap = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    const ArrayElements e = array_elements(b, ptr);
    Value* end = b.build_add(e.head, b.build_zext_i64(len));
    b.build_br_if(b.build_ult(end, b.build_zext_i64(cap)), store, miss);
    b.position_at_end(store);
    Value* at = b.build_add(b.build_add(e.block, b.build_shl(end, iconst(b, 3))), iconst(b, kBronzeHdrBytes));
    Value* new_len = b.build_add(len, b.build_iconst_i32(1));
    array_header_store(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, new_len);
    const auto forget_numbers = [ptr](Builder& bb) {
        Value* word = array_header_load(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
        array_header_store(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                           bb.build_and(word, bb.build_iconst_i32(~int32_t{BRONZE_ABI_ARRAY_BIT_NUMBERS})));
    };
    store_value(b, at, val, kBoxedElementAliasClass, !is_known_non_ref(val), forget_numbers);
    return b.build_bitcast_i64_f64(b.build_sitofp_f64_i32(new_len));
}

// `a.pop()` on an Array at the bare array shape whose last element is
// present: 23.1.3.22's Get, DeletePropertyOrThrow and length Set, which on
// such an array read the element, leave a hole and shorten the length (an
// empty array also takes its head back to 0, as the runtime's pop does). A
// hole, an empty array or any other array goes to `miss`.
Value* PropertyLoweringHelper::lower_array_pop(Builder& b, Value* ptr, BasicBlock* miss) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "apop_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* nonempty = b.append_block(prefix + "_nonempty");
    BasicBlock* present = b.append_block(prefix + "_present");
    BasicBlock* empty = b.append_block(prefix + "_empty");
    BasicBlock* done = b.append_block(prefix + "_done");
    b.position_at_end(cur);
    Value* bare = b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                             tls_block_word(b, BRONZE_TLS_ARRAY_SHAPE_OFF));
    Value* elems = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    Value* has_elems = b.build_eq(b.build_and(elems, tls_block_word(b, BRONZE_TLS_ARRAY_ELEMS_TAG_MASK_OFF)),
                                  iconst(b, kObjectTagBits));
    Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    b.build_br_if(b.build_and(b.build_and(bare, has_elems), b.build_ne(len, b.build_iconst_i32(0))), nonempty, miss);
    b.position_at_end(nonempty);
    Value* new_len = b.build_sub(len, b.build_iconst_i32(1));
    const ArrayElements e = array_elements(b, ptr);
    Value* at = b.build_add(b.build_add(e.block, b.build_shl(b.build_add(e.head, b.build_zext_i64(new_len)), iconst(b, 3))),
                            iconst(b, kBronzeHdrBytes));
    Value* last = with_alias_class(b.build_load(Type::i64(), at, 0), kBoxedElementAliasClass);
    b.build_br_if(b.build_eq(last, iconst(b, BRONZE_ABI_HOLE_BITS)), miss, {}, present, {});
    b.position_at_end(present);
    with_alias_class(b.build_store(Type::i64(), at, 0, iconst(b, BRONZE_ABI_HOLE_BITS)), kBoxedElementAliasClass);
    array_header_store(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET, new_len);
    b.build_br_if(b.build_eq(new_len, b.build_iconst_i32(0)), empty, done);
    b.position_at_end(empty);
    array_header_store(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET, b.build_iconst_i32(0));
    b.build_br(done);
    b.position_at_end(done);
    return last;
}

// An array literal's element `[.., v, ..]` (il_ast.h kLiteralElementWriteMode,
// key `k`): defined on the fresh array, which create.array made `k + 1` long
// or longer with holes, so the element is stored where it is. The runtime's
// own definition (bronze_prop_define) takes anything else: an Array not at the
// bare shape, a class field named by an index.
void PropertyLoweringHelper::lower_literal_element(Builder& b, Value* obj, int32_t k, uint32_t key_index, Value* val,
                                                   Value* site) {
    if (val->type().is_tagged()) val = b.build_bitcast_tagged_i64(val);
    BasicBlock* cur = b.current_block();
    const std::string prefix = "lit_elem_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_store = b.append_block(prefix + "_store");
    BasicBlock* bb_slow = b.append_block(prefix + "_slow");
    BasicBlock* bb_done = b.append_block(prefix + "_done");
    bb_slow->set_cold();

    b.position_at_end(cur);
    Value* ptr = object_address(b, obj);
    b.build_br_if(is_object_address(b, ptr), bb_obj, bb_slow);
    b.position_at_end(bb_obj);
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kArrayHeaderLow)), bb_arr, bb_slow);
    b.position_at_end(bb_arr);
    Value* bare = b.build_eq(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
                             tls_block_word(b, BRONZE_TLS_ARRAY_SHAPE_OFF));
    Value* elems = array_header_load(b, Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    Value* has_elems = b.build_eq(b.build_and(elems, iconst(b, 0xFFFF000000000000ULL)), iconst(b, kObjectTagBits));
    Value* len = array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    Value* in = b.build_ult(b.build_iconst_i32(k), len);
    b.build_br_if(b.build_and(bare, b.build_and(has_elems, in)), bb_store, bb_slow);

    b.position_at_end(bb_store);
    const ArrayElements e = array_elements(b, ptr);
    Value* at = b.build_add(b.build_add(e.block, b.build_shl(b.build_add(iconst(b, static_cast<uint64_t>(k)), e.head),
                                                            iconst(b, 3))),
                            iconst(b, kBronzeHdrBytes));
    const auto forget_numbers = [ptr](Builder& bb) {
        Value* word = array_header_load(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET);
        array_header_store(bb, Type::i32(), ptr, BRONZE_ABI_ARRAY_BITS_OFFSET,
                           bb.build_and(word, bb.build_iconst_i32(~int32_t{BRONZE_ABI_ARRAY_BIT_NUMBERS})));
    };
    store_value(b, at, val, kBoxedElementAliasClass, !is_known_non_ref(val), forget_numbers);
    b.build_br(bb_done);

    b.position_at_end(bb_slow);
    b.build_call("bronze_prop_define", Type::void_type(),
                 {obj, runtime_key(b, key_index), val, site ? site : b.build_iconst_i64(0)});
    b.build_br(bb_done);
    b.position_at_end(bb_done);
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

// `is.plain_array` (il_op.h): an Array still at the thread's bare array shape
// (no named property, no subclass prototype, no integrity level).
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
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    Value* bare = tls_block_word(b, BRONZE_TLS_ARRAY_SHAPE_OFF);
    b.build_br(bb_merge, {b.build_eq(shape, bare)});

    b.position_at_end(bb_merge);
    return result;
}

Value* PropertyLoweringHelper::box_number(Builder& b, Value* f64_val) {
    return box_f64_bits(b, f64_val);
}

} // namespace il2mir
