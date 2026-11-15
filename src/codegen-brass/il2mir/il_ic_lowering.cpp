// The inline-cache paths of a property read and write (il_property.h): the
// way-0 guard-and-load (or store) in front of the helper.

#include "il_abi.h"
#include "il_property.h"
#include "il_speculation.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>

namespace il2mir {

namespace {
constexpr int64_t kIcOwnDataLimit = int64_t{1} << 32;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
static_assert((kPlainHeaderLow & 1) == 1 && BRONZE_ABI_PROTO_EPOCH_FIRST % 2 == 0 && BRONZE_ABI_PROTO_EPOCH_STEP % 2 == 0,
              "the TLS block stands in for a non-object only while no epoch reads as a header");

// A non-object's header and shape words, for a module that runs in this
// process: the zeroed block's header is no object's, so its reads refuse
// every way.
alignas(16) const uint64_t kNotAnObject[4] = {};
} // namespace

IcSite PropertyLoweringHelper::ic_site_ref(Builder& b, uint32_t ic_index) {
    if (ic_index >= ic_site_count_) return {};
    Value* table = nullptr;
    if (table_base_fn_) {
        table = table_base_fn_(b);
    } else {
        table = b.build_func_addr(ic_table_sym_);
        if (module_delta_fn_) table = b.build_add(table, module_delta_fn_(b));
    }
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
    // a NaN-boxed Value whose tag is BRONZE_ABI_TAG_OBJECT, whose header
    // word's low half is (flags << 16) | tag — so one i32 compare against
    // the tag alone says "an object, and a PLAIN one" — the Shape* at
    // BRONZE_ABI_OBJ_SHAPE_OFFSET, and the inline slots from
    // BRONZE_ABI_OBJ_SLOTS_OFFSET. The site's way 0 is InlineCache: the
    // shape at 0 and the (depth << 32 | slot) word at 8, which is below
    // 2^32 exactly when the entry names an own data property — the
    // accessor, absent, f64 and depth bits all live in the high half, so the
    // one unsigned compare refuses them. A slot at or past
    // BRONZE_ABI_OBJ_OVERFLOW_SLOT is in the overflow block, which a shape
    // match proves is as large as the shape needs (slot_base).
    //
    // The guard is ONE branch, not a tag branch followed by a shape
    // branch: the header and shape loads go through a base that a select
    // steers at a mapped block when the value is not an object, so nothing
    // is ever dereferenced through a non-pointer payload (receiver_shape).
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
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, plain, ptr, shape);
    Value* hit = nullptr;
    Value* slot_word = nullptr;
    emit_mono_slot_guard(b, shape, plain, site, hit, slot_word);
    if (spec_) {
        spec_->emit_branch(b, hit, bb_fast, bb_slow, SpecKind::Property, key_tag(key_index));
    } else {
        b.build_br_if(hit, bb_fast, bb_slow);
        b.position_at_end(bb_slow);
    }
    slow_arm(b);
    b.build_br(bb_merge, {lower_prop_get(b, obj, key_index, site)});

    b.position_at_end(bb_fast);
    b.build_br(bb_merge, {load_slot_br(b, ptr, slot_word)});

    b.position_at_end(bb_merge);
    return merge_val;
}

namespace {
// The value a fact is recorded under: `v` behind the bitcasts between `i64`
// and `tagged`.
Value* fact_root(Value* v) {
    for (int hops = 0; hops < 3; ++hops) {
        const Instruction* def = v->defining_instruction();
        if (def == nullptr || def->operand_count() != 1 ||
            (def->opcode() != Opcode::bitcast_i64_tagged && def->opcode() != Opcode::bitcast_tagged_i64)) {
            break;
        }
        v = def->operand(0);
    }
    return v;
}
// The marks of one value a test asks: the latest ones, each a call until
// resolve_fact_markers.
constexpr size_t kProofsAsked = 3;
}  // namespace

void PropertyLoweringHelper::prove_plain(Builder& b, Value* v) {
    if (!spec_ || !spec_->image_sites() || v == nullptr) return;
    const uint32_t id = next_proof_++;
    b.build_call(kFactProofSymbol, Type::void_type(), {b.build_iconst_i32(static_cast<int32_t>(id))});
    plain_proofs_[fact_root(v)].push_back(id);
}

Value* PropertyLoweringHelper::proven_plain(Builder& b, Value* v) {
    if (v == nullptr) return nullptr;
    const auto it = plain_proofs_.find(fact_root(v));
    if (it == plain_proofs_.end()) return nullptr;
    const std::vector<uint32_t>& ids = it->second;
    Value* any = nullptr;
    for (size_t i = ids.size() > kProofsAsked ? ids.size() - kProofsAsked : 0; i < ids.size(); ++i) {
        Value* p = b.build_call(kFactProvenSymbol, Type::i32(), {b.build_iconst_i32(static_cast<int32_t>(ids[i]))});
        any = any ? b.build_or(any, p) : p;
    }
    return any;
}

void PropertyLoweringHelper::site_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, bool exits,
                                         const std::vector<Value*>& fast_args) {
    if (spec_) {
        spec_->site_branch(b, hit, fast, slow, exits, fast_args);
    } else {
        b.build_br_if(hit, fast, fast_args, slow, {});
    }
}

void PropertyLoweringHelper::count_shape_turn(Builder& b, Value* obj, const IcSite& site) {
    if (!site || !shape_turn_fn_ || slow_sites_.size() != 1) return;
    // Zero for a receiver that is no plain object: the harvest reads the
    // word as a shape's address.
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, plain, ptr, shape);
    shape_turn_fn_(b, slow_sites_[0], b.build_select(plain, shape, b.build_iconst_i64(0)));
}

void PropertyLoweringHelper::receiver_shape(Builder& b, Value* obj, Value*& plain, Value*& ptr, Value*& shape) {
    // The payload is the Value with its object tag XORed away, which is below
    // 2^48 exactly when the tag was the object tag.
    ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    // A non-object reads through a mapped block whose header word is no
    // object's, so the header compare alone refuses it and `is_obj` has the
    // select for its one use, which the backend fuses with the compare.
    // Compiled code has the TLS block in its pinned register, whose first
    // word is the even proto epoch (bronze_abi.h); a module that runs in this
    // process under the tiered pipeline (feedback-driven, where any receiver
    // reaches here and the fast interpreter runs it too) uses a static zeroed
    // block of this process instead.
    Value* not_object = in_process_
        ? b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(&kNotAnObject[0])))
        : b.build_pinned_tls_read();
    // Past a block that proved the value plain (PROVEN FACTS) both tests are
    // the constant.
    Value* proven = proven_plain(b, obj);
    Value* base = b.build_select(proven ? b.build_or(proven, is_obj) : is_obj, ptr, not_object);
    plain = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kPlainHeaderLow));
    if (proven) plain = b.build_or(proven, plain);
    shape = b.build_load(Type::i64(), base, BRONZE_ABI_OBJ_SHAPE_OFFSET);
}

void PropertyLoweringHelper::emit_mono_slot_guard(Builder& b, Value* shape, Value* plain, const IcSite& site,
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
    hit = plain ? b.build_and(plain, b.build_and(shape_match, own_data)) : b.build_and(shape_match, own_data);
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
Value* PropertyLoweringHelper::load_slot_br(Builder& b, Value* ptr, Value* slot_word) {
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

void PropertyLoweringHelper::lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val,
                                                 uint32_t imm, const IcSite& site, bool val_is_primitive) {
    count_shape_turn(b, obj, site);
    const bool need_barrier = !val_is_primitive && !is_known_non_ref(val);
    BasicBlock* bb_current = b.current_block();
    Function* fn = bb_current->parent();
    const std::string prefix = "ic_set_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_hit");
    BasicBlock* bb_barrier = need_barrier ? b.append_block(prefix + "_barrier") : nullptr;
    BasicBlock* bb_slow = b.append_block(prefix + "_miss");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(bb_current);
    Value* hit = nullptr;
    Value* ptr = nullptr;
    Value* slot_word = nullptr;
    Value* plain = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, plain, ptr, shape);
    // A Number's store also hits a double slot's way (its bits are the
    // double's); anything else there misses to the helper, which converts an
    // Int32 box or generalizes the slot.
    Value* number_store = b.build_ule(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    emit_mono_slot_guard(b, shape, plain, site, hit, slot_word, number_store);
    // A write's tag is its key after '=', apart from the reads of it.
    const std::string tag = "=" + std::string(key_tag(key_index));
    if (spec_) {
        spec_->emit_branch(b, hit, bb_fast, bb_slow, SpecKind::Property, tag);
    } else {
        b.build_br_if(hit, bb_fast, bb_slow);
        b.position_at_end(bb_slow);
    }
    slow_arm(b);
    lower_prop_set(b, obj, key_index, val, imm, site_ptr(b, site));
    b.build_br(bb_merge);

    // The store, then the barrier for a value that is not a Number (every
    // reference is tagged above BRONZE_ABI_NUMBER_MAX_BITS), given the slot's
    // own address as every C++ slot store gives it (HeapValue).
    b.position_at_end(bb_fast);
    Value* base = slot_base(b, ptr, slot_word);
    if (need_barrier) {
        Value* slot_addr = b.build_add(b.build_add(base, b.build_shl(slot_word, b.build_iconst_i64(3))),
                                       b.build_iconst_i64(kBronzeObjSlotsOffset));
        with_alias_class(b.build_store(Type::i64(), slot_addr, 0, val), kObjectSlotAliasClass);
        Value* is_ref = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
        b.build_br_if(is_ref, bb_barrier, bb_merge);
        b.position_at_end(bb_barrier);
        b.build_write_barrier(slot_addr, val);
        b.build_br(bb_merge);
    } else {
        with_alias_class(b.build_store_indexed(Type::i64(), base, slot_word, 8, kBronzeObjSlotsOffset, val), kObjectSlotAliasClass);
        b.build_br(bb_merge);
    }

    b.position_at_end(bb_merge);
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
