// The static-slot fast path (il.h `staticSlot`): a property site whose class
// layout proved its key's instance slot reads or writes that slot at a
// COMPILE-TIME constant offset behind one guard, in front of the site's
// ordinary inline-cache sequence rather than instead of it.
//
// In front, because a proven site can still be reached by an object of some
// other shape — a plain literal, a subclass whose constructor took a branch the
// layout did not model, an instance a `delete` or a `defineProperty` moved to
// a new shape — and those keep the cache hit they had. What a hit no longer
// pays is the cache's slot word: its load, its own-data test, and the
// inline-or-overflow select, all of which the slot number answers here, once,
// when the code is built.
//
// The guard asks one of two questions of the receiver's shape word:
//
//   identity  `shape == cell`, the module cell this site owns. The runtime
//             publishes a shape there only after checking that the key really
//             is an own, data (and for a store writable) property of that
//             shape at exactly this slot (runtime/static_shape.cpp).
//   family    `stamp - (base + lo) <=u span`: the shape's family stamp names a
//             class in the site's `extends` subtree, which the runtime wrote
//             only after verifying that class's whole field list is a prefix
//             of the shape, name by name and slot by slot
//             (runtime/class_family.cpp).
//
// Either way nothing upstream — the layout, the receiver's typing — has to be
// sound for this to be correct: a wrong claim is a cell that never publishes
// or a stamp that never matches, and the site keeps taking its cache. The
// failure mode is a cost, never an answer.

#include "il_abi.h"
#include "il_lowering.h"
#include "il_property.h"
#include <brass/mir/function.hpp>
#include <algorithm>

namespace il2mir {

namespace {
constexpr int32_t kFamilyOffset = BRONZE_ABI_SHAPE_FAMILY_OFFSET;
constexpr int32_t kDoubleSlotsOffset = BRONZE_ABI_SHAPE_DOUBLESLOTS_OFFSET;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
// `__bronze_family_base` is the base id and then two zero words. Seen from
// eight bytes in, shifted back by the family offset, those two words sit where
// a shape keeps its family stamp and its double-slot map: a stand-in shape for
// a receiver that is no plain object, whose stamp no family range admits and
// whose every slot is boxed, so a guard can read through it unconditionally.
static_assert(kDoubleSlotsOffset == kFamilyOffset + 8, "the stand-in shape's two words are adjacent");
constexpr int64_t kStandInShapeDelta = 8 - kFamilyOffset;
// A guard's miss block is named cold so that brass's block layout
// (codegen/block_layout.cpp, which reads the name) puts the hit on the
// fallthrough: the miss is the cache's sequence, laid out as its own trace.
constexpr const char* kMissName = "_miss_cold";

Value* iconst(Builder& b, uint64_t v) {
    return b.build_iconst_i64(static_cast<int64_t>(v));
}

// The stand-in shape's address, and the receiver's shape word read as an
// address too, for the select between them: a data address is a `ptr`, and a
// select takes two values of one type.
Value* stand_in_shape(Builder& b, Value* family_base) {
    return b.build_add(family_base, b.build_iconst_i64(kStandInShapeDelta));
}
Value* safe_shape_of(Builder& b, Value* plain, Value* recv_base, Value* family_base) {
    Value* shape = b.build_load(Type::ptr(), recv_base, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    return b.build_select(plain, shape, stand_in_shape(b, family_base));
}
} // namespace

Value* PropertyLoweringHelper::data_addr(Builder& b, const std::string& sym) {
    Value* addr = b.build_func_addr(sym);
    return module_delta_fn_ ? b.build_add(addr, module_delta_fn_(b)) : addr;
}

Value* PropertyLoweringHelper::static_guard(Builder& b, Value* obj, const std::vector<StaticGuardTerm>& terms,
                                            Value*& ptr) {
    Value* plain = nullptr;
    Value* shape = nullptr;
    Value* recv_base = nullptr;
    receiver_shape(b, obj, plain, ptr, shape, &recv_base);
    Value* ok = plain;
    Value* cells = nullptr;
    Value* family_base = nullptr;
    Value* safe_shape = nullptr;
    // A word read off the shape through `safe_shape`, which is the shape when
    // the receiver is plain and the stand-in otherwise: every term can then be
    // one more `and` before the guard's single branch.
    auto shape_word = [&](int32_t off) {
        if (!safe_shape) {
            family_base = data_addr(b, family_base_sym_);
            safe_shape = safe_shape_of(b, plain, recv_base, family_base);
        }
        return b.build_load(Type::i64(), safe_shape, off);
    };
    Value* stamp = nullptr;
    Value* first_id = nullptr;
    Value* doubles = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<uint32_t> cells_asked;
    for (const StaticGuardTerm& t : terms) {
        const StaticSlotSite& site = t.site;
        if (site.family()) {
            // One compare per distinct range: two sites of one class ask the
            // same question of the same word.
            const std::pair<uint32_t, uint32_t> range{site.family_lo, site.family_span};
            if (std::find(ranges.begin(), ranges.end(), range) == ranges.end()) {
                ranges.push_back(range);
                if (!stamp) {
                    stamp = shape_word(kFamilyOffset);
                    first_id = b.build_load(Type::i64(), family_base, 0);
                }
                Value* rel = b.build_sub(stamp, b.build_add(first_id, iconst(b, site.family_lo)));
                ok = b.build_and(ok, b.build_ule(rel, iconst(b, site.family_span)));
            }
        } else if (std::find(cells_asked.begin(), cells_asked.end(), site.cell) == cells_asked.end()) {
            cells_asked.push_back(site.cell);
            if (!cells) cells = data_addr(b, static_cells_sym_);
            Value* want = b.build_load(Type::i64(), cells, static_cast<int32_t>(site.cell * sizeof(uint64_t)));
            ok = b.build_and(ok, b.build_eq(shape, want));
        }
        // A store asks what the shape says the slot's eight bytes ARE. A slot
        // the shape calls an f64 (slot_repr.h) takes a bare store of a
        // Number, whose box is the double's bits, and of nothing else: a
        // pointer there would be read as a double and never traced. A value
        // lowering knows is a Number needs no test, nor does a slot past the
        // map's width, which is never a double one.
        if (t.store && !t.store_is_number && site.slot < BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT) {
            if (!doubles) doubles = shape_word(kDoubleSlotsOffset);
            Value* boxed = b.build_eq(b.build_and(doubles, iconst(b, uint64_t{1} << site.slot)), iconst(b, 0));
            Value* is_num = b.build_ule(t.store, iconst(b, kBronzeNumberMaxBits));
            ok = b.build_and(ok, b.build_or(boxed, is_num));
        }
    }
    return ok;
}

// Inline slots are the object's own words from BRONZE_ABI_OBJ_SLOTS_OFFSET;
// the rest live in the overflow block, an Object-tagged Value whose payload is
// a flat Value array after its header. No bound and no capacity is tested: a
// shape that carries slot N is a shape whose objects were grown to hold it
// before the shape word was stored (setProp's ensureSlots ordering), and the
// guard has agreed the receiver carries such a shape.
Value* PropertyLoweringHelper::static_slot_base(Builder& b, Value* ptr, uint32_t slot, int32_t& off) {
    if (slot < kBronzeObjInlineSlots) {
        off = static_cast<int32_t>(kBronzeObjSlotsOffset + slot * 8);
        return ptr;
    }
    off = static_cast<int32_t>(kBronzeHdrBytes + (slot - kBronzeObjInlineSlots) * 8);
    return b.build_and(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset), iconst(b, kPayloadMask));
}

void PropertyLoweringHelper::store_with_barrier(Builder& b, Value* base, int32_t off, Value* val, bool need_barrier,
                                                const std::string& prefix) {
    if (!need_barrier) {
        b.build_store(Type::i64(), base, off, val);
        return;
    }
    // The barrier for a value that is not a Number (every reference is tagged
    // above BRONZE_ABI_NUMBER_MAX_BITS), handed the slot's own address as
    // every C++ slot store hands it.
    Value* slot_addr = b.build_add(base, b.build_iconst_i64(off));
    b.build_store(Type::i64(), slot_addr, 0, val);
    BasicBlock* bb_cur = b.current_block();
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_cont = b.append_block(prefix + "_stored");
    b.position_at_end(bb_cur);
    b.build_br_if(b.build_ugt(val, iconst(b, kBronzeNumberMaxBits)), bb_barrier, bb_cont);
    b.position_at_end(bb_barrier);
    b.build_call("brass_gc_write_barrier", Type::void_type(), {slot_addr, val});
    b.build_br(bb_cont);
    b.position_at_end(bb_cont);
}

void PropertyLoweringHelper::emit_static_fill(Builder& b, Value* obj, uint32_t key_index, const StaticSlotSite& site,
                                              bool for_write) {
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "static_fill_" + std::to_string(bb_cur->parent()->next_block_id());
    // Named cold, which brass's block layout reads (codegen/block_layout.cpp):
    // a fill runs once per site or shape, and is no part of any trace.
    BasicBlock* bb_call = b.append_block(prefix + "_call_cold");
    BasicBlock* bb_cont = b.append_block(prefix + "_done");
    b.position_at_end(bb_cur);
    if (site.family()) {
        // The stamp is a fact about the SHAPE, so the one-shot test is on the
        // shape: a site that meets five subclasses stamps five shapes once
        // each and then hits on all five, where a cell would pin the first.
        Value* plain = nullptr;
        Value* ptr = nullptr;
        Value* shape = nullptr;
        Value* recv_base = nullptr;
        receiver_shape(b, obj, plain, ptr, shape, &recv_base);
        Value* safe = safe_shape_of(b, plain, recv_base, data_addr(b, family_base_sym_));
        Value* unstamped = b.build_eq(b.build_load(Type::i64(), safe, kFamilyOffset),
                                      iconst(b, BRONZE_ABI_FAMILY_UNSTAMPED));
        b.build_br_if(b.build_and(plain, unstamped), bb_call, bb_cont);
        b.position_at_end(bb_call);
        b.build_call("bronze_family_stamp", Type::void_type(), {obj});
    } else {
        // Only while the cell is still zero, which makes it one-shot both
        // ways: a site that published stops calling, and a site whose layout
        // was wrong holds the refusal marker and stops too.
        Value* cells = data_addr(b, static_cells_sym_);
        const int32_t off = static_cast<int32_t>(site.cell * sizeof(uint64_t));
        b.build_br_if(b.build_eq(b.build_load(Type::i64(), cells, off), iconst(b, 0)), bb_call, bb_cont);
        b.position_at_end(bb_call);
        b.build_call("bronze_static_shape_publish", Type::void_type(),
                     {obj, runtime_key(b, key_index), b.build_add(cells, b.build_iconst_i64(off)),
                      b.build_iconst_i32(static_cast<int32_t>(site.slot)), b.build_iconst_i32(for_write ? 1 : 0)});
    }
    b.build_br(bb_cont);
    bb_call->set_cold();
    b.position_at_end(bb_cont);
}

Value* PropertyLoweringHelper::lower_prop_get_static(Builder& b, Value* obj, uint32_t key_index,
                                                     const StaticSlotSite& site, const StaticMiss& miss) {
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "static_get_" + std::to_string(bb_cur->parent()->next_block_id());
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_miss = b.append_block(prefix + kMissName);
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* merge_val = b.add_block_param(bb_merge, Type::i64());

    b.position_at_end(bb_cur);
    Value* ptr = nullptr;
    b.build_br_if(static_guard(b, obj, {StaticGuardTerm{site}}, ptr), bb_hit, bb_miss);

    b.position_at_end(bb_hit);
    int32_t off = 0;
    Value* base = static_slot_base(b, ptr, site.slot, off);
    // No correction of any kind: the cell or the stamp stands for the key
    // being an own data property of this shape at exactly this slot, so the
    // eight bytes are what [[Get]] answers. A double slot's word is the
    // number's box.
    b.build_br(bb_merge, {b.build_load(Type::i64(), base, off)});

    b.position_at_end(bb_miss);
    Value* slow_val = miss.get(b);
    // The access may have collected, so the receiver's bits are read again.
    emit_static_fill(b, miss.receiver(b), key_index, site, /*for_write=*/false);
    b.build_br(bb_merge, {slow_val});

    b.position_at_end(bb_merge);
    return merge_val;
}

void PropertyLoweringHelper::lower_prop_set_static(Builder& b, Value* obj, uint32_t key_index, Value* val,
                                                   const StaticSlotSite& site, bool val_is_number, bool need_barrier,
                                                   const StaticMiss& miss) {
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "static_set_" + std::to_string(bb_cur->parent()->next_block_id());
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_miss = b.append_block(prefix + kMissName);
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");

    b.position_at_end(bb_cur);
    Value* ptr = nullptr;
    StaticGuardTerm term{site};
    term.store = val;
    term.store_is_number = val_is_number;
    b.build_br_if(static_guard(b, obj, {term}, ptr), bb_hit, bb_miss);

    b.position_at_end(bb_hit);
    int32_t off = 0;
    Value* base = static_slot_base(b, ptr, site.slot, off);
    store_with_barrier(b, base, off, val, need_barrier, prefix);
    b.build_br(bb_merge);

    b.position_at_end(bb_miss);
    miss.set(b);
    emit_static_fill(b, miss.receiver(b), key_index, site, /*for_write=*/true);
    b.build_br(bb_merge);

    b.position_at_end(bb_merge);
}

void IlLowering::register_class_families(Builder& b, Value* map_addr) {
    if (options_.class_family_count == 0) return;
    // Preorder ids, contiguous per subtree: the runtime allocates the module's
    // run of them and writes the first into the base word every family guard
    // loads (bronze_abi.h, bronze_register_class_family).
    b.build_call("bronze_register_class_family", Type::void_type(),
                 {b.build_func_addr(module_sym("__bronze_class_families")),
                  b.build_iconst_i32(static_cast<int32_t>(options_.class_family_count)),
                  b.build_func_addr(module_sym("__bronze_class_family_fields")), map_addr,
                  module_data_addr(b, "__bronze_family_base")});
}

} // namespace il2mir
