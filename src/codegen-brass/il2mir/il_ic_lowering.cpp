// The inline-cache paths of a property read and write (il_property.h): the
// way-0 guard-and-load in front of the helper, the read's scan of its other
// ways, and the store.

#include "il_abi.h"
#include "il_property.h"
#include "il_speculation.h"
#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <cstdlib>

namespace il2mir {

namespace {
constexpr int32_t kIcShapeOffset = BRONZE_ABI_IC_SHAPE_OFFSET;
constexpr int32_t kIcSlotWordOffset = BRONZE_ABI_IC_SLOT_OFFSET;
constexpr int32_t kIcEntrySize = static_cast<int32_t>(BRONZE_ABI_IC_ENTRY_SIZE);
constexpr int64_t kIcOwnDataLimit = int64_t{1} << 32;
constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
constexpr int64_t kPayloadLimit = int64_t{1} << 48;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;

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
    return {table, static_cast<int32_t>(static_cast<uint64_t>(ic_index) * kBronzeIcSiteSize)};
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

Value* PropertyLoweringHelper::lower_prop_get(Builder& b, Value* obj, uint32_t key_index, Value* ic_entry) {
    Value* entry = ic_entry ? ic_entry : b.build_iconst_i64(0);
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
    // one unsigned compare refuses them. A slot past
    // BRONZE_ABI_OBJ_INLINE_SLOTS is in the overflow block, which a shape
    // match proves is as large as the shape needs (slot_base).
    //
    // The guard is ONE branch, not a tag branch followed by a shape
    // branch: the header and shape loads go through a base that a select
    // steers at a mapped block when the value is not an object, so nothing
    // is ever dereferenced through a non-pointer payload (receiver_shape).
    // Straight-line code before a single two-way split keeps the site to
    // few new blocks with no critical edge, which is what keeps GVN-PRE's
    // per-hoist restart from going quadratic over a function with hundreds
    // of property reads.
    //
    // A way-0 miss does not go straight to the helper: the POLY block
    // compares the receiver's shape against ways 1..N-1 and loads the slot on
    // a hit, so a site the helper filled with 2..N shapes stays inline in
    // every tier. Only a receiver no way describes reaches `bronze_prop_get`.
    // Under feedback the two tests are two speculation sites: way 0
    // (SpecKind::Property, counting poly-block entries) and the scan
    // (SpecKind::PolyProperty, counting helper calls), so tier 2 pins a
    // monomorphic site to way 0 and guards a polymorphic one on the scan,
    // deoptimizing only when it turns megamorphic.
    //
    // In the JIT's own tiers (in process, with feedback) the scan is a call
    // (kPolyScanHelper) and the miss a counted helper, which is what keeps a
    // read to about forty instructions; tier 2 expands the scan back inline
    // (il_ic_stub.h), so its code is what the inline form always was.
    // BRONZE_NO_POLY_SCAN=1: no poly block, a way-0 miss calls the helper
    // (the A/B seam).
    static const bool no_poly_scan_env = [] {
        const char* v = std::getenv("BRONZE_NO_POLY_SCAN");
        return v && v[0] == '1';
    }();
    // The scan goes where a site can turn out polymorphic: every read under
    // the tiered pipeline, and in an AOT build only the reads its profile
    // saw miss way 0 past the cold fills. A plain AOT build's inline reads
    // are the ones inference proved monomorphic, and the scan's blocks
    // there cost the whole-program optimizer (tests/cli
    // large_module_compile_test: 3000 top-level reads went from under 90 s
    // to 213 s) for nothing.
    bool no_poly_scan = no_poly_scan_env;
    if (!no_poly_scan && !in_process_) {
        const std::optional<uint32_t> way0 =
            spec_ ? spec_->peek_profile(SpecKind::Property, key_tag(key_index)) : std::nullopt;
        no_poly_scan = !way0 || !SpecSiteEmitter::profile_says_polymorphic(*way0);
        // The profile's scan site for this read is then not lowered; the
        // next read of the same key keeps its own.
        if (no_poly_scan && spec_) spec_->skip_site(SpecKind::PolyProperty, key_tag(key_index));
    }
    const bool compact = small_forms_ && !no_poly_scan && in_process_ && spec_ && spec_->has_feedback();
    BasicBlock* bb_current = b.current_block();
    Function* fn = bb_current->parent();
    const std::string prefix = "ic_get_" + std::to_string(fn->next_block_id());

    BasicBlock* bb_fast = b.append_block(prefix + "_hit");
    BasicBlock* bb_poly = no_poly_scan ? nullptr : b.append_block(prefix + "_poly");
    BasicBlock* bb_slow = b.append_block(prefix + "_miss");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* merge_val = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(bb_current);
    Value* hit = nullptr;
    Value* ptr = nullptr;
    Value* slot_word = nullptr;
    mono_hit(b, obj, site, hit, ptr, slot_word);
    BasicBlock* bb_way0_miss = bb_poly ? bb_poly : bb_slow;
    Value* c0 = nullptr;
    if (spec_) {
        spec_->emit_branch(b, hit, bb_fast, bb_way0_miss, SpecKind::Property, key_tag(key_index),
                           compact ? &c0 : nullptr);
    } else {
        b.build_br_if(hit, bb_fast, bb_way0_miss);
        b.position_at_end(bb_way0_miss);
    }

    if (compact) {
        // append_block moves the insertion point: the block first.
        BasicBlock* bb_poly_hit = b.append_block(prefix + "_phit");
        b.position_at_end(bb_poly);
        Value* entry = site_ptr(b, site);
        Value* addr = b.build_call(kPolyScanHelper, Type::i64(), {obj, entry, c0});
        Value* c1 = nullptr;
        spec_->emit_branch(b, b.build_ne(addr, b.build_iconst_i64(0)), bb_poly_hit, bb_slow,
                           SpecKind::PolyProperty, key_tag(key_index), &c1);
        b.build_br(bb_merge, {b.build_call("bronze_prop_get_counted", Type::i64(),
                                           {obj, runtime_key(b, key_index), entry, c1})});
        b.position_at_end(bb_poly_hit);
        b.build_br(bb_merge, {b.build_load(Type::i64(), addr, 0)});
    } else {
        if (bb_poly) {
            // Recomputed from `obj`, so what a way-0 guard carries into the
            // poly block is the operands alone, as it was into the helper.
            // Carrying the way-0 block's receiver words instead put them in
            // every armed way-0 guard's state, which kept them materialized
            // in tier-2 code (about 50 ms of nbody's 350 in bro's
            // cpu_work.js).
            //
            // A site whose way 1 is empty has never held a second shape (the
            // helper installs move-to-front), so its miss goes straight to
            // the helper: one load and branch instead of the scan.
            BasicBlock* bb_scan = b.append_block(prefix + "_scan");
            BasicBlock* bb_poly_hit = b.append_block(prefix + "_phit");
            b.position_at_end(bb_poly);
            Value* way1_shape = b.build_load(Type::i64(), site.base, site.off + kIcEntrySize + kIcShapeOffset);
            b.build_br_if(b.build_ne(way1_shape, b.build_iconst_i64(0)), bb_scan, bb_slow);
            b.position_at_end(bb_scan);
            Value* poly_hit = nullptr;
            Value* poly_slot = nullptr;
            Value* poly_plain = nullptr;
            Value* poly_ptr = nullptr;
            Value* poly_shape = nullptr;
            receiver_shape(b, obj, poly_plain, poly_ptr, poly_shape);
            other_ways_hit(b, poly_plain, poly_shape, site, poly_hit, poly_slot);
            if (spec_) {
                spec_->emit_branch(b, poly_hit, bb_poly_hit, bb_slow, SpecKind::PolyProperty, key_tag(key_index));
            } else {
                b.build_br_if(poly_hit, bb_poly_hit, bb_slow);
                b.position_at_end(bb_slow);
            }
            b.position_at_end(bb_poly_hit);
            Value* poly_val = b.build_load_indexed(Type::i64(), slot_base(b, poly_ptr, poly_slot), poly_slot, 8,
                                                   kBronzeObjSlotsOffset);
            b.build_br(bb_merge, {poly_val});
            b.position_at_end(bb_slow);
        }
        Value* slow_val =
            b.build_call("bronze_prop_get", Type::i64(), {obj, runtime_key(b, key_index), site_ptr(b, site)});
        b.build_br(bb_merge, {slow_val});
    }

    b.position_at_end(bb_fast);
    Value* fast_val = b.build_load_indexed(Type::i64(), slot_base(b, ptr, slot_word), slot_word, 8,
                                           kBronzeObjSlotsOffset);
    b.build_br(bb_merge, {fast_val});

    b.position_at_end(bb_merge);
    return merge_val;
}

void PropertyLoweringHelper::other_ways_hit(Builder& b, Value* plain, Value* shape, const IcSite& site, Value*& hit,
                                            Value*& slot_word) {
    Value* any = nullptr;
    slot_word = nullptr;
    // At most one way names a shape (an install rewrites an existing entry
    // in place), so the words select on the shape alone and the one
    // own-data test is of the selected word.
    for (int32_t way = BRONZE_ABI_IC_WAYS - 1; way >= 1; --way) {
        const int32_t base = site.off + way * kIcEntrySize;
        Value* match = b.build_eq(shape, b.build_load(Type::i64(), site.base, base + kIcShapeOffset));
        Value* word = b.build_load(Type::i64(), site.base, base + kIcSlotWordOffset);
        slot_word = slot_word ? b.build_select(match, word, slot_word) : word;
        any = any ? b.build_or(any, match) : match;
    }
    hit = b.build_and(b.build_and(plain, any), b.build_ult(slot_word, b.build_iconst_i64(kIcOwnDataLimit)));
}

void PropertyLoweringHelper::receiver_shape(Builder& b, Value* obj, Value*& plain, Value*& ptr, Value*& shape) {
    // The payload is the Value with its object tag XORed away, which is below
    // 2^48 exactly when the tag was the object tag.
    ptr = b.build_xor(obj, b.build_iconst_i64(static_cast<int64_t>(kObjectTagBits)));
    Value* is_obj = b.build_ult(ptr, b.build_iconst_i64(kPayloadLimit));
    // A non-object reads through a mapped block whose words refuse the hit.
    // Compiled code has the TLS block in its pinned register; a module that
    // runs in this process under the tiered pipeline (feedback-driven, where
    // any receiver reaches here and the fast interpreter runs it too) uses a
    // static zeroed block of this process instead, whose header word is no
    // object's, so its own header compare refuses it and needs no `is_obj`.
    Value* not_object = in_process_
        ? b.build_iconst_i64(static_cast<int64_t>(reinterpret_cast<uintptr_t>(&kNotAnObject[0])))
        : b.build_pinned_tls_read();
    Value* base = b.build_select(is_obj, ptr, not_object);
    Value* is_plain = b.build_eq(b.build_load(Type::i32(), base, 0), b.build_iconst_i32(kPlainHeaderLow));
    shape = b.build_load(Type::i64(), base, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    plain = in_process_ ? is_plain : b.build_and(is_obj, is_plain);
}

void PropertyLoweringHelper::mono_hit(Builder& b, Value* obj, const IcSite& site, Value*& hit, Value*& ptr,
                                      Value*& slot_word) {
    Value* plain = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, plain, ptr, shape);
    Value* cached_shape = b.build_load(Type::i64(), site.base, site.off + kIcShapeOffset);
    Value* shape_match = b.build_eq(shape, cached_shape);
    slot_word = b.build_load(Type::i64(), site.base, site.off + kIcSlotWordOffset);
    Value* own_data = b.build_ult(slot_word, b.build_iconst_i64(kIcOwnDataLimit));
    hit = b.build_and(plain, b.build_and(shape_match, own_data));
}

// The base that, with slot_word * 8 + BRONZE_ABI_OBJ_SLOTS_OFFSET, addresses
// slot `slot_word` of the plain object at `ptr`: the object for an inline
// slot, else its overflow block shifted back by the inline slots and the
// block's header.
Value* PropertyLoweringHelper::slot_base(Builder& b, Value* ptr, Value* slot_word) {
    Value* is_inline = b.build_ult(slot_word, b.build_iconst_i64(kBronzeObjInlineSlots));
    Value* overflow = b.build_and(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset),
                                  b.build_iconst_i64(static_cast<int64_t>(kPayloadMask)));
    constexpr int64_t kShift = kBronzeObjSlotsOffset + kBronzeObjInlineSlots * 8 - kBronzeHdrBytes;
    Value* shifted = b.build_sub(overflow, b.build_iconst_i64(kShift));
    return b.build_select(is_inline, ptr, shifted);
}

Value* PropertyLoweringHelper::load_slot(Builder& b, Value* ptr, Value* slot_word) {
    return b.build_load_indexed(Type::i64(), slot_base(b, ptr, slot_word), slot_word, 8, kBronzeObjSlotsOffset);
}

void PropertyLoweringHelper::expand_poly_scan(Builder& b, Instruction* call, Value*& hit, Value*& ptr,
                                              Value*& slot_word) {
    Value* obj = call->operand(0);
    Value* entry = call->operand(1);
    Value* counter = call->operand(2);
    const Instruction* counter_def = counter ? counter->defining_instruction() : nullptr;
    if (counter_def && counter_def->opcode() == Opcode::iconst_i64 && counter_def->imm_i64() != 0) {
        Value* old = b.build_load(Type::i32(), counter, 0);
        b.build_store(Type::i32(), counter, 0, b.build_add(old, b.build_iconst_i32(1)));
    }
    Value* plain = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, plain, ptr, shape);
    other_ways_hit(b, plain, shape, IcSite{entry, 0}, hit, slot_word);
}

void PropertyLoweringHelper::lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val,
                                                 uint32_t imm, const IcSite& site) {
    const bool compact = small_forms_ && in_process_ && spec_ && spec_->has_feedback();
    BasicBlock* bb_current = b.current_block();
    Function* fn = bb_current->parent();
    const std::string prefix = "ic_set_" + std::to_string(fn->next_block_id());
    BasicBlock* bb_fast = b.append_block(prefix + "_hit");
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_slow = b.append_block(prefix + "_miss");
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(bb_current);
    Value* hit = nullptr;
    Value* ptr = nullptr;
    Value* slot_word = nullptr;
    mono_hit(b, obj, site, hit, ptr, slot_word);
    Value* counter = nullptr;
    if (spec_) {
        // A write's tag is its key after '=', apart from the reads of it.
        spec_->emit_branch(b, hit, bb_fast, bb_slow, SpecKind::Property, "=" + std::string(key_tag(key_index)),
                           compact ? &counter : nullptr);
    } else {
        b.build_br_if(hit, bb_fast, bb_slow);
        b.position_at_end(bb_slow);
    }
    if (compact) {
        b.build_call("bronze_prop_set_counted", Type::void_type(),
                     {obj, runtime_key(b, key_index), val, site_ptr(b, site), b.build_iconst_i32(imm != 0 ? 1 : 0),
                      counter});
    } else {
        lower_prop_set(b, obj, key_index, val, imm, site_ptr(b, site));
    }
    b.build_br(bb_merge);

    // The store, then the barrier for a value that is not a Number (every
    // reference is tagged above BRONZE_ABI_NUMBER_MAX_BITS), given the slot's
    // own address as every C++ slot store gives it (HeapValue).
    b.position_at_end(bb_fast);
    Value* base = slot_base(b, ptr, slot_word);
    Value* slot_addr = b.build_add(b.build_add(base, b.build_shl(slot_word, b.build_iconst_i64(3))),
                                   b.build_iconst_i64(kBronzeObjSlotsOffset));
    b.build_store(Type::i64(), slot_addr, 0, val);
    Value* is_ref = b.build_ugt(val, b.build_iconst_i64(static_cast<int64_t>(kBronzeNumberMaxBits)));
    b.build_br_if(is_ref, bb_barrier, bb_merge);
    b.position_at_end(bb_barrier);
    b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

void PropertyLoweringHelper::lower_prop_set(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm,
                                            Value* ic_entry) {
    // The fourth operand is the site pointer and nothing else: a site the
    // module has no table entry for passes null, never an index.
    Value* entry = ic_entry ? ic_entry : b.build_iconst_i64(0);
    Value* strict_val = b.build_iconst_i32(imm != 0 ? 1 : 0);
    b.build_call("bronze_prop_set", Type::void_type(), {obj, runtime_key(b, key_index), val, entry, strict_val});
}

void PropertyLoweringHelper::lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure) {
    b.build_call("bronze_method_def", Type::void_type(), {obj, runtime_key(b, key_index), closure});
}

} // namespace il2mir
