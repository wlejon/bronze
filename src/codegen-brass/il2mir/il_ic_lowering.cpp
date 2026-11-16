// The inline-cache paths of a property read and write (il_property.h): the
// way-0 guard-and-load (or store) in front of the helper.

#include "il_abi.h"
#include "il_property.h"
#include "il_prop_stubs.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

namespace il2mir {

namespace {
constexpr int64_t kIcOwnDataLimit = int64_t{1} << 32;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;

// A mapped block a non-object's header words are read through, for a module
// that runs in this process: its words are never trusted (the object test
// is), only loaded. Four words: a function's code word (fn.code_is) is the
// deepest read through it.
alignas(16) const uint64_t kNotAnObject[4] = {};
} // namespace

IcSite PropertyLoweringHelper::ic_site_ref(Builder& b, uint32_t ic_index) {
    if (ic_index >= ic_site_count_) return {};
    Value* table = b.build_func_addr(ic_table_sym_);
    if (module_delta_fn_) table = b.build_add(table, module_delta_fn_(b));
    return {table, static_cast<int32_t>(static_cast<uint64_t>(ic_index) * kBronzeIcSiteSize), ic_index};
}

// The site's packed way-0 row (bronze_abi_layout.h, "the HOT array"): the
// runtime copies way 0's shape and slot words there on every fill, so the
// guard's two loads come from a line holding four sites' rows rather than
// one site's four ways.
int32_t PropertyLoweringHelper::hot_row_off(const IcSite& site) const {
    return static_cast<int32_t>(ic_hot_table_offset(ic_site_count_) +
                                static_cast<int64_t>(site.index) * BRONZE_ABI_IC_HOT_ENTRY_SIZE);
}

// A row has way 0's layout (bronze_abi_layout.h).
static_assert(BRONZE_ABI_IC_HOT_SHAPE_OFFSET == BRONZE_ABI_IC_SHAPE_OFFSET &&
                  BRONZE_ABI_IC_HOT_SLOT_OFFSET == BRONZE_ABI_IC_SLOTWORD_OFFSET,
              "a row reads as the first two words of way 0");

Value* PropertyLoweringHelper::way0_shape(Builder& b, const IcSite& site) {
    return b.build_load(Type::i64(), site.base, hot_row_off(site) + BRONZE_ABI_IC_HOT_SHAPE_OFFSET);
}

Value* PropertyLoweringHelper::way0_slot_word(Builder& b, const IcSite& site) {
    return b.build_load(Type::i64(), site.base, hot_row_off(site) + BRONZE_ABI_IC_HOT_SLOT_OFFSET);
}

Value* PropertyLoweringHelper::site_ptr(Builder& b, const IcSite& site) {
    if (site.off == 0) return site.base;
    return b.build_add(site.base, b.build_iconst_i64(site.off));
}

Value* PropertyLoweringHelper::ic_site(Builder& b, uint32_t ic_index) {
    const IcSite site = ic_site_ref(b, ic_index);
    return site ? site_ptr(b, site) : nullptr;
}

// The runtime's id for the module's key constant `key_index`, read through
// the module's key map; kNoKey passes through as itself.
Value* PropertyLoweringHelper::runtime_key(Builder& b, uint32_t key_index) {
    if (key_index == kNoKey) return b.build_iconst_i32(static_cast<int32_t>(key_index));
    Value* map_addr = b.build_func_addr(key_map_sym_);
    return b.build_load(Type::i32(), map_addr, static_cast<int32_t>(key_index * sizeof(uint32_t)));
}

Value* PropertyLoweringHelper::lower_prop_get(Builder& b, Value* obj, uint32_t key_index, const IcSite& site) {
    Value* entry = site ? site_ptr(b, site) : b.build_iconst_i64(0);
    return b.build_call("bronze_prop_get", Type::i64(), {obj, runtime_key(b, key_index), entry});
}

Value* PropertyLoweringHelper::lower_prop_get_mono(Builder& b, Value* obj, uint32_t key_index, const IcSite& site) {
    // The bronze object model as the inline hit reads it (bronze_abi.h):
    // a NaN-boxed Value whose tag is BRONZE_ABI_TAG_OBJECT, the Shape* at
    // BRONZE_ABI_OBJ_SHAPE_OFFSET of EVERY object kind (heap.h carriesShape:
    // a plain object, an array, a function, a proxy's sentinel alike, so no
    // kind test), and the inline slots from BRONZE_ABI_OBJ_SLOTS_OFFSET. The site's way 0 is InlineCache: the
    // shape at 0 and the (depth << 32 | slot) word at 8, which is below
    // 2^32 exactly when the entry names an own data property — the
    // accessor, absent, f64 and depth bits all live in the high half, so the
    // one unsigned compare refuses them. A slot at or past
    // BRONZE_ABI_OBJ_OVERFLOW_SLOT is in the overflow block, which a shape
    // match proves is as large as the shape needs (slot_base).
    //
    // The guard is ONE branch, not a tag branch followed by a shape
    // branch: the shape load goes through a base that a select steers at a
    // mapped block when the value is not an object, so nothing is ever
    // dereferenced through a non-pointer payload (receiver_shape).
    // A way-0 miss is `bronze_prop_get`, which answers from the site's
    // other ways and fills it.
    count_shape_turn(b, obj, site);
    BasicBlock* bb_current = b.current_block();
    Function* fn = bb_current->parent();
    const std::string prefix = "ic_get_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_hit");
    BasicBlock* bb_slow = b.append_block(prefix + "_miss");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* merge_val = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(bb_current);
    Value* is_object = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, is_object, ptr, shape);
    Value* hit = nullptr;
    Value* slot_word = nullptr;
    emit_mono_slot_guard(b, shape, is_object, site, hit, slot_word);
    b.build_br_if(hit, bb_fast, bb_slow);
    b.position_at_end(bb_slow);
    slow_arm(b);
    b.build_br(bb_merge, {b.build_call(stub_sym(kPropGetStub), Type::i64(),
                                       {obj, runtime_key(b, key_index), site_ptr(b, site)})});

    b.position_at_end(bb_fast);
    b.build_br(bb_merge, {load_slot_br(b, ptr, slot_word)});

    b.position_at_end(bb_merge);
    return merge_val;
}

// `x.length`. An Array's `length` is an own data property no program can
// make an accessor (10.4.2: non-configurable), so the header's length word is
// the answer for every Array; a String's is its code-unit count; a typed
// array's comes from its header when the view and its prototype chain are
// pristine (typed_array_chain_pristine), as the runtime's own read decides.
// Everything else, and a typed array that is not, is the cache's.
Value* PropertyLoweringHelper::lower_length_get(Builder& b, Value* obj, uint32_t key_index, const IcSite& site) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "len_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_arr = b.append_block(prefix + "_arr");
    BasicBlock* bb_notarr = b.append_block(prefix + "_notarr");
    BasicBlock* bb_view = b.append_block(prefix + "_view");
    BasicBlock* bb_view_len = b.append_block(prefix + "_viewlen");
    BasicBlock* bb_notobj = b.append_block(prefix + "_notobj");
    BasicBlock* bb_str = b.append_block(prefix + "_str");
    BasicBlock* bb_other = b.append_block(prefix + "_other");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* result = b.add_block_param(bb_merge, Type::i64());
    constexpr int32_t kArrayLow = (BRONZE_ABI_OBJ_FLAGS_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    constexpr int32_t kViewLow = (BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY << 16) | BRONZE_ABI_TAG_OBJECT;
    // A uint32 length as a Number's bits.
    auto number = [&](Value* len) { return b.build_bitcast_i64_f64(b.build_sitofp_f64_i64(b.build_zext_i64(len))); };

    b.position_at_end(cur);
    Value* ptr = object_address(b, obj);
    b.build_br_if(is_object_address(b, ptr), bb_obj, bb_notobj);

    b.position_at_end(bb_obj);
    Value* header = b.build_load(Type::i32(), ptr, 0);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kArrayLow)), bb_arr, bb_notarr);
    b.position_at_end(bb_arr);
    arm_begin(b, kLengthArmArray);
    b.build_br(bb_merge, {number(array_header_load(b, Type::i32(), ptr, BRONZE_ABI_ARRAY_LENGTH_OFFSET))});

    b.position_at_end(bb_notarr);
    b.build_br_if(b.build_eq(header, b.build_iconst_i32(kViewLow)), bb_view, bb_other);
    b.position_at_end(bb_view);
    Value* kind = with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_KIND_OFFSET), kTypedArrayHeaderAliasClass);
    b.build_br_if(typed_array_chain_pristine(b, ptr, b.build_zext_i64(kind)), bb_view_len, bb_other);
    b.position_at_end(bb_view_len);
    arm_begin(b, kLengthArmView);
    b.build_br(bb_merge, {number(with_alias_class(b.build_load(Type::i32(), ptr, BRONZE_ABI_TA_LENGTH_OFFSET),
                                                  kTypedArrayHeaderAliasClass))});

    b.position_at_end(bb_notobj);
    Value* tag = b.build_lshr(obj, b.build_iconst_i64(BRONZE_ABI_VALUE_TAG_SHIFT));
    b.build_br_if(b.build_eq(tag, b.build_iconst_i64(BRONZE_ABI_TAG_STRING)), bb_str, bb_other);
    b.position_at_end(bb_str);
    arm_begin(b, kLengthArmString);
    Value* text = b.build_and(obj, b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    b.build_br(bb_merge, {number(b.build_load(Type::i32(), text, BRONZE_ABI_STRING_LENGTH_OFFSET))});

    b.position_at_end(bb_other);
    b.build_br(bb_merge, {lower_prop_get_mono(b, obj, key_index, site)});
    b.position_at_end(bb_merge);
    return result;
}

void PropertyLoweringHelper::count_shape_turn(Builder& b, Value* obj, const IcSite& site) {
    if (!site || !shape_turn_fn_ || slow_sites_.size() != 1) return;
    // Zero for a receiver that is no object: the harvest reads the word as a
    // shape's address.
    Value* is_object = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, is_object, ptr, shape);
    shape_turn_fn_(b, slow_sites_[0], b.build_select(is_object, shape, b.build_iconst_i64(0)));
}

void PropertyLoweringHelper::count_callee_turn(Builder& b, Value* code) {
    if (!shape_turn_fn_ || slow_sites_.size() != 1) return;
    shape_turn_fn_(b, slow_sites_[0], b.build_or(code, b.build_iconst_i64(int64_t{1} << kLastCalleeBit)));
}

void PropertyLoweringHelper::receiver_shape(Builder& b, Value* obj, Value*& is_object, Value*& ptr, Value*& shape) {
    // The payload is the Value with its object tag XORed away, which is below
    // 2^48 exactly when the tag was the object tag.
    ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    // A non-object reads its shape word through a mapped block instead, and
    // `is_object` — the guard's one kind fact, since every object's word at
    // BRONZE_ABI_OBJ_SHAPE_OFFSET is a shape and an empty way (shape 0) or a
    // sentinel shape matches no object's — refuses it.
    // Compiled code has the TLS block in its pinned register; a module that
    // runs in this process under the tiered pipeline (feedback-driven, where
    // any receiver reaches here and the fast interpreter runs it too) uses a
    // static zeroed block of this process instead.
    is_object = is_obj;
    shape = b.build_load(Type::i64(), object_or_blank(b, ptr, is_object), BRONZE_ABI_OBJ_SHAPE_OFFSET);
}

Value* PropertyLoweringHelper::object_or_blank(Builder& b, Value* ptr, Value* is_object) {
    Value* not_object = in_process_
        ? b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(&kNotAnObject[0])))
        : b.build_pinned_tls_read();
    return b.build_select(is_object, ptr, not_object);
}

void PropertyLoweringHelper::emit_mono_slot_guard(Builder& b, Value* shape, Value* is_object, const IcSite& site,
                                                  Value*& hit, Value*& slot_word, Value* number_store) {
    Value* cached_shape = way0_shape(b, site);
    Value* shape_match = b.build_eq(shape, cached_shape);
    slot_word = way0_slot_word(b, site);
    Value* own_data = b.build_ult(slot_word, b.build_iconst_i64(kIcOwnDataLimit));
    if (number_store) {
        // A double slot's entry (BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG): a Number's
        // bits are the double's, so a store of one is the same word.
        Value* dbl = b.build_eq(b.build_lshr(slot_word, b.build_iconst_i64(32)),
                                b.build_iconst_i64(BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG));
        own_data = b.build_or(own_data, b.build_and(dbl, number_store));
        slot_word = b.build_and(slot_word, b.build_iconst_i64(0xFFFFFFFFLL));
    }
    hit = is_object ? b.build_and(is_object, b.build_and(shape_match, own_data)) : b.build_and(shape_match, own_data);
}

// The base that, with slot_word * 8 + BRONZE_ABI_OBJ_SLOTS_OFFSET, addresses
// slot `slot_word` of the plain object at `ptr`: the object for an inline
// slot, else its overflow block shifted back by the inline slots and the
// block's header.
Value* PropertyLoweringHelper::slot_base(Builder& b, Value* ptr, Value* slot_word) {
    Value* is_inline = b.build_ult(slot_word, b.build_iconst_i64(kBronzeObjOverflowSlot));
    Value* overflow = b.build_and(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset),
                                  b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    constexpr int64_t kShift = kBronzeObjSlotsOffset + kBronzeObjOverflowSlot * 8 - kBronzeHdrBytes;
    Value* shifted = b.build_sub(overflow, b.build_iconst_i64(kShift));
    return b.build_select(is_inline, ptr, shifted);
}

Value* PropertyLoweringHelper::load_slot(Builder& b, Value* ptr, Value* slot_word) {
    return b.build_load_indexed(Type::i64(), slot_base(b, ptr, slot_word), slot_word, 8, kBronzeObjSlotsOffset);
}

// The same read as a predicted branch on the slot word, which a site's cache
// way keeps the same from one hit to the next: an inline slot is one load off
// the object, and the overflow block is read only for a slot that is in it,
// with no select in the address the value is loaded through. The builder
// leaves in the join.
Value* PropertyLoweringHelper::load_slot_at(Builder& b, Value* ptr, Value* slot_word) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "slot_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_in = b.append_block(prefix + "_inline");
    BasicBlock* bb_out = b.append_block(prefix + "_overflow");
    BasicBlock* bb_join = b.append_block(prefix + "_value");
    Value* val = b.add_block_param(bb_join, Type::i64());
    b.position_at_end(cur);
    b.build_br_if(b.build_ult(slot_word, b.build_iconst_i64(kBronzeObjOverflowSlot)), bb_in, bb_out);
    b.position_at_end(bb_in);
    b.build_br(bb_join, {b.build_load_indexed(Type::i64(), ptr, slot_word, 8, kBronzeObjSlotsOffset)});
    b.position_at_end(bb_out);
    Value* overflow = b.build_and(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset),
                                  b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    constexpr int64_t kShift = kBronzeObjSlotsOffset + kBronzeObjOverflowSlot * 8 - kBronzeHdrBytes;
    b.build_br(bb_join, {b.build_load_indexed(Type::i64(), overflow, slot_word, 8, kBronzeObjSlotsOffset - kShift)});
    b.position_at_end(bb_join);
    return val;
}

Value* PropertyLoweringHelper::slot_address(Builder& b, Value* ptr, Value* slot_word) {
    BasicBlock* cur = b.current_block();
    const std::string prefix = "slotat_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_out = b.append_block(prefix + "_overflow");
    BasicBlock* bb_join = b.append_block(prefix + "_base");
    Value* base = b.add_block_param(bb_join, Type::i64());
    b.position_at_end(cur);
    b.build_br_if(b.build_ult(slot_word, b.build_iconst_i64(kBronzeObjOverflowSlot)), bb_join, {ptr}, bb_out, {});
    b.position_at_end(bb_out);
    Value* overflow = b.build_and(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset),
                                  b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    constexpr int64_t kShift = kBronzeObjSlotsOffset + kBronzeObjOverflowSlot * 8 - kBronzeHdrBytes;
    b.build_br(bb_join, {b.build_sub(overflow, b.build_iconst_i64(kShift))});
    b.position_at_end(bb_join);
    return b.build_add(b.build_add(base, b.build_shl(slot_word, b.build_iconst_i64(3))),
                       b.build_iconst_i64(kBronzeObjSlotsOffset));
}

void PropertyLoweringHelper::lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val,
                                                 uint32_t imm, const IcSite& site, bool val_is_primitive) {
    count_shape_turn(b, obj, site);
    const bool need_barrier = !val_is_primitive && !is_known_non_ref(val);
    BasicBlock* bb_current = b.current_block();
    Function* fn = bb_current->parent();
    const std::string prefix = "ic_set_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_hit");
    BasicBlock* bb_slow = b.append_block(prefix + "_miss");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(bb_current);
    Value* hit = nullptr;
    Value* ptr = nullptr;
    Value* slot_word = nullptr;
    Value* is_object = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, is_object, ptr, shape);
    // A Number's store also hits a double slot's way (its bits are the
    // double's); anything else there misses to the helper, which converts an
    // Int32 box or generalizes the slot.
    Value* number_store = b.build_ule(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    emit_mono_slot_guard(b, shape, is_object, site, hit, slot_word, number_store);
    b.build_br_if(hit, bb_fast, bb_slow);
    b.position_at_end(bb_slow);
    slow_arm(b);
    b.build_call(stub_sym(kPropSetStub), Type::void_type(),
                 {obj, runtime_key(b, key_index), val, site_ptr(b, site), b.build_iconst_i32(static_cast<int32_t>(imm))});
    b.build_br(bb_merge);

    // The store, then the barrier for a value that is not a Number (every
    // reference is tagged above BRONZE_ABI_NUMBER_MAX_BITS), given the slot's
    // own address as every C++ slot store gives it (HeapValue).
    b.position_at_end(bb_fast);
    Value* base = slot_base(b, ptr, slot_word);
    if (need_barrier) {
        Value* slot_addr = b.build_add(b.build_add(base, b.build_shl(slot_word, b.build_iconst_i64(3))),
                                       b.build_iconst_i64(kBronzeObjSlotsOffset));
        store_value(b, slot_addr, val, kObjectSlotAliasClass, /*need_barrier=*/true);
        b.build_br(bb_merge);
    } else {
        with_alias_class(b.build_store_indexed(Type::i64(), base, slot_word, 8, kBronzeObjSlotsOffset, val), kObjectSlotAliasClass);
        b.build_br(bb_merge);
    }

    b.position_at_end(bb_merge);
}

void PropertyLoweringHelper::store_value(Builder& b, Value* addr, Value* val, uint8_t alias_class, bool need_barrier,
                                         const std::function<void(Builder&)>& on_non_number) {
    with_alias_class(b.build_store(Type::i64(), addr, 0, val), alias_class);
    if (!need_barrier && !on_non_number) return;
    BasicBlock* cur = b.current_block();
    const std::string prefix = "store_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_other = b.append_block(prefix + "_ref");
    BasicBlock* bb_join = b.append_block(prefix + "_done");
    b.position_at_end(cur);
    // Every reference is tagged above BRONZE_ABI_NUMBER_MAX_BITS, and so is
    // every value that is not a Number in its double form.
    Value* other = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(other, bb_other, bb_join);
    b.position_at_end(bb_other);
    if (on_non_number) on_non_number(b);
    if (need_barrier) b.build_write_barrier(addr, val);
    b.build_br(bb_join);
    b.position_at_end(bb_join);
}

void PropertyLoweringHelper::store_value_indexed(Builder& b, Value* base, Value* index, int32_t offset, Value* val,
                                                 uint8_t alias_class, bool need_barrier,
                                                 const std::function<void(Builder&)>& on_non_number) {
    with_alias_class(b.build_store_indexed(Type::i64(), base, index, 8, offset, val), alias_class);
    if (!need_barrier && !on_non_number) return;
    BasicBlock* cur = b.current_block();
    const std::string prefix = "store_" + std::to_string(cur->parent()->next_block_id());
    BasicBlock* bb_other = b.append_block(prefix + "_ref");
    BasicBlock* bb_join = b.append_block(prefix + "_done");
    b.position_at_end(cur);
    Value* other = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(other, bb_other, bb_join);
    b.position_at_end(bb_other);
    if (on_non_number) on_non_number(b);
    if (need_barrier) {
        Value* addr = b.build_add(b.build_add(base, b.build_shl(index, b.build_iconst_i64(3))),
                                  b.build_iconst_i64(offset));
        b.build_write_barrier(addr, val);
    }
    b.build_br(bb_join);
    b.position_at_end(bb_join);
}

void PropertyLoweringHelper::lower_prop_set(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm,
                                            Value* ic_entry) {
    // The fourth operand is the site pointer and nothing else: a site the
    // module has no table entry for passes null, never an index.
    Value* entry = ic_entry ? ic_entry : b.build_iconst_i64(0);
    if (imm == kDefineWriteMode || imm == kLiteralElementWriteMode) {
        // A definition's miss: never the assignment's helper,
        // whose chain walk would run an inherited setter. The entry is this
        // site's own, which only bronze_prop_define fills. An array
        // literal's element misses the same way (il.h kWriteLiteralElement).
        b.build_call("bronze_prop_define", Type::void_type(), {obj, runtime_key(b, key_index), val, entry});
        return;
    }
    Value* strict_val = b.build_iconst_i32(imm != 0 ? 1 : 0);
    b.build_call("bronze_prop_set", Type::void_type(), {obj, runtime_key(b, key_index), val, entry, strict_val});
}

void PropertyLoweringHelper::lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure) {
    b.build_call("bronze_method_def", Type::void_type(), {obj, runtime_key(b, key_index), closure});
}

} // namespace il2mir
