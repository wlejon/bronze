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
#include "il_property_lowering.h"
#include <brass/mir/function.hpp>
#include <algorithm>
#include <stdexcept>

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

// The shape memo's values on one edge into a site's join (StaticMiss).
void append_memo(std::vector<Value*>& args, const std::function<std::vector<Value*>(Builder&)>& hook, Builder& b) {
    if (!hook) return;
    for (Value* v : hook(b)) args.push_back(v);
}

// The join's parameters for them, handed back to the memo, which stands for
// the receivers' shapes past the site whichever edge came in.
void merge_memo(Builder& b, BasicBlock* merge, size_t n, const StaticMiss& miss) {
    if (!miss.memo_merged) return;
    std::vector<Value*> params;
    for (size_t i = 0; i < n; ++i) params.push_back(b.add_block_param(merge, Type::i64()));
    miss.memo_merged(params);
}

// A static guard's loads, each under the alias class of the word it reads
// (il_abi.h), so that a slot store between two sites clobbers none of them.
Value* header_word(Builder& b, Value* ptr) {
    return with_alias_class(b.build_load(Type::i32(), ptr, 0), kShapeWordAliasClass);
}
Value* shape_word_of(Builder& b, Value* ptr) {
    return with_alias_class(b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET), kShapeWordAliasClass);
}
Value* runtime_word(Builder& b, Value* base, int32_t off) {
    return with_alias_class(b.build_load(Type::i64(), base, off), kRuntimeWordAliasClass);
}

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
    // The guard tables' addresses, once per function (IlLowering's hoisting):
    // every static site of a function reads one of the two.
    if (static_base_fn_ && !sym.empty() && (sym == static_cells_sym_ || sym == family_base_sym_)) {
        return static_base_fn_(b, sym == family_base_sym_);
    }
    return unshared_data_addr(b, sym);
}

Value* PropertyLoweringHelper::unshared_data_addr(Builder& b, const std::string& sym) {
    Value* addr = b.build_func_addr(sym);
    return module_delta_fn_ ? b.build_add(addr, module_delta_fn_(b)) : addr;
}

Value* PropertyLoweringHelper::not_plain_shape(Builder& b) {
    // In this process, a zeroed block of ours.
    alignas(16) static const uint64_t kZeroShape[(kDoubleSlotsOffset + 8) / 8] = {};
    if (in_process_) return iconst(b, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&kZeroShape[0])));
    // Compiled ahead, the module's family base seen as a shape (its two zero
    // words after the base id), as an i64: MIR has no pointer-to-integer
    // conversion but a pointer difference, taken here against the null
    // pointer the base's last word holds, and built by hand since
    // Builder::build_sub types its result as its first operand.
    Value* family_base = data_addr(b, family_base_sym_);
    Value* null = b.build_load(Type::ptr(), family_base, 16);
    Instruction* diff = b.arena().make<Instruction>(Opcode::sub, Type::i64());
    diff->add_operand(stand_in_shape(b, family_base));
    diff->add_operand(null);
    Value* addr = b.create_value(Type::i64());
    addr->set_defining_instruction(diff);
    diff->set_result(addr);
    b.insert(diff);
    return addr;
}

Value* PropertyLoweringHelper::keyed_shape(Builder& b, Value* obj) {
    // Two predicted branches, not selects: a plain object's path is the
    // object test, the header test and the shape load, with no select in the
    // address the header and shape are read through, and anything else takes
    // a cold block that hands the join the stand-in.
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "keyed_" + std::to_string(bb_cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_other = b.append_block(prefix + "_other_cold");
    BasicBlock* bb_join = b.append_block(prefix + "_join");
    Value* keyed = b.add_block_param(bb_join, Type::i64());
    bb_other->set_cold();
    b.position_at_end(bb_cur);
    Value* ptr = b.build_xor(obj, iconst(b, uint64_t{BRONZE_ABI_TAG_OBJECT} << 48));
    b.build_br_if(b.build_ult(ptr, iconst(b, uint64_t{1} << 48)), bb_obj, bb_other);
    b.position_at_end(bb_obj);
    constexpr int32_t kPlainHeader = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
    Value* plain = b.build_eq(header_word(b, ptr), b.build_iconst_i32(kPlainHeader));
    Value* shape = shape_word_of(b, ptr);
    b.build_br_if(plain, bb_join, {shape}, bb_other, {});
    b.position_at_end(bb_other);
    b.build_br(bb_join, {not_plain_shape(b)});
    b.position_at_end(bb_join);
    return keyed;
}

Value* PropertyLoweringHelper::keyed_shape_flat(Builder& b, Value* obj) {
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    receiver_shape(b, obj, plain, ptr, shape);
    return b.build_select(plain, shape, not_plain_shape(b));
}

void PropertyLoweringHelper::site_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, bool exits) {
    if (spec_) {
        spec_->site_branch(b, hit, fast, slow, exits);
    } else {
        b.build_br_if(hit, fast, slow);
    }
}

Value* PropertyLoweringHelper::plain_shape_or_miss(Builder& b, Value* obj, BasicBlock* miss, bool exits) {
    // The payload is the Value with its object tag XORed away, below 2^48
    // exactly when the tag was the object tag; only then is the header read.
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "recv_" + std::to_string(bb_cur->parent()->next_block_id());
    BasicBlock* bb_obj = b.append_block(prefix + "_obj");
    BasicBlock* bb_plain = b.append_block(prefix + "_plain");
    b.position_at_end(bb_cur);
    Value* ptr = b.build_xor(obj, iconst(b, uint64_t{BRONZE_ABI_TAG_OBJECT} << 48));
    site_branch(b, b.build_ult(ptr, iconst(b, uint64_t{1} << 48)), bb_obj, miss, exits);
    b.position_at_end(bb_obj);
    constexpr int32_t kPlainHeader = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
    site_branch(b, b.build_eq(header_word(b, ptr), b.build_iconst_i32(kPlainHeader)), bb_plain, miss,
                exits);
    b.position_at_end(bb_plain);
    return shape_word_of(b, ptr);
}

Value* PropertyLoweringHelper::family_id(Builder& b, Value* keyed) {
    Value* family_base = data_addr(b, family_base_sym_);
    return b.build_sub(runtime_word(b, keyed, kFamilyOffset), runtime_word(b, family_base, 0));
}

bool PropertyLoweringHelper::layout_number(const StaticSlotSite& site) const {
    if (layout_cells_ == nullptr || site.none() || site.family()) return false;
    const auto layout = layout_cells_->find(site.cell);
    if (layout == layout_cells_->end()) return false;
    for (const LayoutCell::Pair& p : layout->second.pairs) {
        if (p.slot == site.slot) return p.number;
    }
    return false;
}

Value* PropertyLoweringHelper::number_flag(Value* v) const {
    for (int hops = 0; v != nullptr && hops < 3; ++hops) {
        const auto it = number_flags_.find(v);
        if (it != number_flags_.end()) return it->second;
        const Instruction* def = v->defining_instruction();
        if (def == nullptr || def->operand_count() != 1 ||
            (def->opcode() != Opcode::bitcast_i64_tagged && def->opcode() != Opcode::bitcast_tagged_i64)) {
            return nullptr;
        }
        v = def->operand(0);
    }
    return nullptr;
}

Value* PropertyLoweringHelper::static_guard(Builder& b, Value* obj, Value* keyed,
                                            const std::vector<StaticGuardTerm>& terms, Value*& ptr,
                                            Value** family_id_io) {
    // The payload, meaningful where the guard holds: an object's tag XORed
    // away. Every term below admits only a plain object (a cell or a family
    // stamp names a plain shape, and a non-plain receiver's keyed shape is
    // none of them), so `ok` needs no separate plainness term.
    ptr = b.build_xor(obj, iconst(b, uint64_t{BRONZE_ABI_TAG_OBJECT} << 48));
    Value* ok = nullptr;
    auto all = [&](Value* term) { ok = ok ? b.build_and(ok, term) : term; };
    Value* cells = nullptr;
    // A word read off the keyed shape, which is always a readable shape: a
    // plain receiver's own, or for anything else the stand-in
    // (not_plain_shape), whose stamp no family range admits and whose every
    // slot is boxed. So no term needs plainness ANDed in, and every term is
    // one more `and` before the guard's single branch.
    auto shape_word = [&](int32_t off) { return runtime_word(b, keyed, off); };
    Value* fid = family_id_io ? *family_id_io : nullptr;
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
                if (!fid) {
                    fid = family_id(b, keyed);
                    if (family_id_io) *family_id_io = fid;
                }
                // One family is an equality: a compare against the id, where
                // the range form costs a subtract and a test as well.
                if (site.family_span == 0) {
                    all(b.build_eq(fid, iconst(b, site.family_lo)));
                } else {
                    Value* rel = site.family_lo == 0 ? fid : b.build_sub(fid, iconst(b, site.family_lo));
                    all(b.build_ule(rel, iconst(b, site.family_span)));
                }
            }
        } else if (std::find(cells_asked.begin(), cells_asked.end(), site.cell) == cells_asked.end()) {
            cells_asked.push_back(site.cell);
            if (!cells) cells = data_addr(b, static_cells_sym_);
            Value* want = runtime_word(b, cells, static_cast<int32_t>(site.cell * sizeof(uint64_t)));
            all(b.build_eq(keyed, want));
        }
        // A store asks what the shape says the slot's eight bytes ARE. A slot
        // the shape calls an f64 (slot_repr.h) takes a bare store of a
        // Number, whose box is the double's bits, and of nothing else: a
        // pointer there would be read as a double and never traced. A value
        // lowering knows is a Number needs no test, nor does a slot past the
        // map's width, which is never a double one.
        if (t.store && !t.store_is_number && site.slot < BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT) {
            // A layout cell's `number` member is a double slot on every shape
            // the cell admits: the value alone is the question.
            if (layout_number(site)) {
                all(b.build_ule(t.store, iconst(b, kBronzeNumberMaxBits)));
                continue;
            }
            if (!doubles) doubles = shape_word(kDoubleSlotsOffset);
            Value* boxed = b.build_eq(b.build_and(doubles, iconst(b, uint64_t{1} << site.slot)), iconst(b, 0));
            Value* is_num = b.build_ule(t.store, iconst(b, kBronzeNumberMaxBits));
            all(b.build_or(boxed, is_num));
        }
    }
    if (!ok) throw std::logic_error("il2mir: a static guard with no site term");
    return ok;
}

// Inline slots are the object's own words from BRONZE_ABI_OBJ_SLOTS_OFFSET;
// the rest live in the overflow block, an Object-tagged Value whose payload is
// a flat Value array after its header. No bound and no capacity is tested: a
// shape that carries slot N is a shape whose objects were grown to hold it
// before the shape word was stored (setProp's ensureSlots ordering), and the
// guard has agreed the receiver carries such a shape.
Value* PropertyLoweringHelper::static_slot_base(Builder& b, Value* ptr, uint32_t slot, int32_t& off) {
    if (slot < kBronzeObjOverflowSlot) {
        off = static_cast<int32_t>(kBronzeObjSlotsOffset + slot * 8);
        return ptr;
    }
    off = static_cast<int32_t>(kBronzeHdrBytes + (slot - kBronzeObjOverflowSlot) * 8);
    return b.build_and(with_alias_class(b.build_load(Type::i64(), ptr, kBronzeObjOverflowOffset), kShapeWordAliasClass),
                       iconst(b, kPayloadMask));
}

void PropertyLoweringHelper::store_with_barrier(Builder& b, Value* base, int32_t off, Value* val, bool need_barrier,
                                                const std::string& prefix) {
    if (!need_barrier) {
        with_alias_class(b.build_store(Type::i64(), base, off, val), kObjectSlotAliasClass);
        return;
    }
    // The barrier for a value that is not a Number (every reference is tagged
    // above BRONZE_ABI_NUMBER_MAX_BITS), handed the slot's own address as
    // every C++ slot store hands it.
    Value* slot_addr = b.build_add(base, b.build_iconst_i64(off));
    with_alias_class(b.build_store(Type::i64(), slot_addr, 0, val), kObjectSlotAliasClass);
    BasicBlock* bb_cur = b.current_block();
    BasicBlock* bb_barrier = b.append_block(prefix + "_barrier");
    BasicBlock* bb_cont = b.append_block(prefix + "_stored");
    b.position_at_end(bb_cur);
    b.build_br_if(b.build_ugt(val, iconst(b, kBronzeNumberMaxBits)), bb_barrier, bb_cont);
    b.position_at_end(bb_barrier);
    b.build_write_barrier(slot_addr, val);
    b.build_br(bb_cont);
    b.position_at_end(bb_cont);
}

// An identity cell published by the first receiver that held the key at the
// claimed slot, while that object was still being built: three.js's
// `properties.get(material)` record is read in setProgram once before its
// last key (`receiveShadow`) is added, and its binding states before
// `attributesNum`, so the cell held a shape no later receiver has and every
// read missed. A receiver whose shape grew from the cell's, one or two keys
// on, clears the cell and publishes again; the runtime's checks are the same
// as for a first fill (bronze_static_shape_publish), and the cell only ever
// moves to a descendant, so a site that still meets the older shape does not
// make it change back and forth.
void PropertyLoweringHelper::emit_static_regrow(Builder& b, Value* obj, Value* cell, Value* cells, int32_t off,
                                                BasicBlock* bb_publish, BasicBlock* bb_done, const std::string& prefix) {
    // append_block moves the insertion point.
    BasicBlock* bb_cur = b.current_block();
    BasicBlock* bb_obj = b.append_block(prefix + "_grown_obj_cold");
    BasicBlock* bb_plain = b.append_block(prefix + "_grown_plain_cold");
    BasicBlock* bb_up = b.append_block(prefix + "_grown_up_cold");
    BasicBlock* bb_up2 = b.append_block(prefix + "_grown_up2_cold");
    BasicBlock* bb_reset = b.append_block(prefix + "_grown_reset_cold");
    for (BasicBlock* bb : {bb_cur, bb_obj, bb_plain, bb_up, bb_up2, bb_reset}) bb->set_cold();
    b.position_at_end(bb_cur);
    b.build_br_if(b.build_eq(b.build_lshr(obj, iconst(b, 48)), iconst(b, BRONZE_ABI_TAG_OBJECT)), bb_obj, bb_done);
    b.position_at_end(bb_obj);
    Value* ptr = b.build_lshr(b.build_shl(obj, iconst(b, 16)), iconst(b, 16));
    constexpr int32_t kPlainLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
    b.build_br_if(b.build_eq(b.build_load(Type::i32(), ptr, 0), b.build_iconst_i32(kPlainLow)), bb_plain, bb_done);
    b.position_at_end(bb_plain);
    Value* shape = b.build_load(Type::i64(), ptr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    Value* parent = b.build_load(Type::i64(), shape, BRONZE_ABI_SHAPE_PARENT_OFFSET);
    b.build_br_if(b.build_eq(parent, cell), bb_reset, bb_up);
    b.position_at_end(bb_up);
    b.build_br_if(b.build_eq(parent, iconst(b, 0)), bb_done, bb_up2);
    b.position_at_end(bb_up2);
    Value* grand = b.build_load(Type::i64(), parent, BRONZE_ABI_SHAPE_PARENT_OFFSET);
    b.build_br_if(b.build_eq(grand, cell), bb_reset, bb_done);
    b.position_at_end(bb_reset);
    b.build_store(Type::i64(), cells, off, iconst(b, 0));
    b.build_br(bb_publish);
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
        Value* family_base = data_addr(b, family_base_sym_);
        Value* safe = safe_shape_of(b, plain, recv_base, family_base);
        Value* unstamped = b.build_eq(b.build_load(Type::i64(), safe, kFamilyOffset),
                                      iconst(b, BRONZE_ABI_FAMILY_UNSTAMPED));
        b.build_br_if(b.build_and(plain, unstamped), bb_call, bb_cont);
        b.position_at_end(bb_call);
        // The site's own range goes along, absolute, so a shape two unrelated
        // classes both describe is stamped with the class this site expects.
        Value* lo = b.build_add(b.build_load(Type::i64(), family_base, 0), iconst(b, site.family_lo));
        b.build_call("bronze_family_stamp", Type::void_type(), {obj, lo, iconst(b, site.family_span)});
    } else {
        // Only while the cell is still zero, which makes it one-shot both
        // ways: a site that published stops calling, and a site whose layout
        // was wrong holds the refusal marker and stops too.
        Value* cells = data_addr(b, static_cells_sym_);
        const int32_t off = static_cast<int32_t>(site.cell * sizeof(uint64_t));
        Value* cell = b.build_load(Type::i64(), cells, off);
        if (site.regrow) {
            BasicBlock* bb_grown = b.append_block(prefix + "_grown_cold");
            b.position_at_end(bb_cur);
            b.build_br_if(b.build_eq(cell, iconst(b, 0)), bb_call, bb_grown);
            b.position_at_end(bb_grown);
            emit_static_regrow(b, obj, cell, cells, off, bb_call, bb_cont, prefix);
        } else {
            b.position_at_end(bb_cur);
            b.build_br_if(b.build_eq(cell, iconst(b, 0)), bb_call, bb_cont);
        }
        b.position_at_end(bb_call);
        const auto layout = layout_cells_ ? layout_cells_->find(site.cell) : decltype(layout_cells_->end()){};
        if (layout_cells_ && layout != layout_cells_->end()) {
            // A LAYOUT CELL (il.h `Module::layoutCells`) stands for every
            // member's claim: the whole list goes, three u32 a member (its
            // key as the runtime knows it, its slot, its flags: stored-to,
            // a double), on the stack.
            const std::vector<LayoutCell::Pair>& pairs = layout->second.pairs;
            Value* list = b.build_alloca(static_cast<uint32_t>(pairs.size() * 12), 4);
            for (size_t i = 0; i < pairs.size(); ++i) {
                const int32_t at = static_cast<int32_t>(i * 12);
                b.build_store(Type::i32(), list, at, runtime_key(b, pairs[i].key));
                b.build_store(Type::i32(), list, at + 4, b.build_iconst_i32(static_cast<int32_t>(pairs[i].slot)));
                const uint32_t flags = (pairs[i].write ? BRONZE_ABI_LAYOUT_PAIR_WRITE : 0u) |
                                       (pairs[i].number ? BRONZE_ABI_LAYOUT_PAIR_DOUBLE : 0u);
                b.build_store(Type::i32(), list, at + 8, b.build_iconst_i32(static_cast<int32_t>(flags)));
            }
            b.build_call("bronze_static_layout_publish", Type::void_type(),
                         {obj, list, b.build_iconst_i32(static_cast<int32_t>(pairs.size())),
                          b.build_iconst_i32(static_cast<int32_t>(layout->second.key_count)),
                          b.build_add(cells, b.build_iconst_i64(off))});
        } else {
            b.build_call("bronze_static_shape_publish", Type::void_type(),
                         {obj, runtime_key(b, key_index), b.build_add(cells, b.build_iconst_i64(off)),
                          b.build_iconst_i32(static_cast<int32_t>(site.slot)), b.build_iconst_i32(for_write ? 1 : 0)});
        }
    }
    b.build_br(bb_cont);
    bb_call->set_cold();
    b.position_at_end(bb_cont);
}

namespace {
// A static site's guard: against the keyed shape the caller holds, or, with
// none in hand, on a path only a plain receiver reaches (the object and
// header tests branching to the site's miss), which hands its shape to the
// memo first.
Value* guard_or_miss(Builder& b, PropertyLoweringHelper& pl, Value* obj, const std::vector<StaticGuardTerm>& terms,
                     const StaticMiss& miss, BasicBlock* bb_miss, Value*& ptr) {
    Value* family = miss.family;
    Value* shape = miss.keyed;
    if (!shape) {
        shape = pl.plain_shape_or_miss(b, obj, bb_miss, miss.exits);
        if (miss.memo_fresh) miss.memo_fresh(shape);
    }
    Value* ok = pl.static_guard(b, obj, shape, terms, ptr, &family);
    if (family != miss.family && miss.memo_family) miss.memo_family(family);
    return ok;
}
}  // namespace

Value* PropertyLoweringHelper::lower_prop_get_static(Builder& b, Value* obj, uint32_t key_index,
                                                     const StaticSlotSite& site, const StaticMiss& miss) {
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "static_get_" + std::to_string(bb_cur->parent()->next_block_id());
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_miss = b.append_block(prefix + kMissName);
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    Value* merge_val = b.add_block_param(bb_merge, Type::i64());
    // The read's NUMBER FLAG (il_property.h), the join's second parameter.
    const bool number = layout_number(site);
    Value* merge_flag = number ? b.add_block_param(bb_merge, Type::i32()) : nullptr;
    const size_t values = number ? 2 : 1;

    b.position_at_end(bb_cur);
    Value* ptr = nullptr;
    site_branch(b, guard_or_miss(b, *this, obj, {StaticGuardTerm{site}}, miss, bb_miss, ptr), bb_hit, bb_miss,
                miss.exits);

    b.position_at_end(bb_hit);
    int32_t off = 0;
    Value* base = static_slot_base(b, ptr, site.slot, off);
    // No correction of any kind: the cell or the stamp stands for the key
    // being an own data property of this shape at exactly this slot, so the
    // eight bytes are what [[Get]] answers. A double slot's word is the
    // number's box.
    std::vector<Value*> hit_args{with_alias_class(b.build_load(Type::i64(), base, off), kObjectSlotAliasClass)};
    if (number) hit_args.push_back(b.build_iconst_i32(1));
    append_memo(hit_args, miss.memo_hit, b);
    b.build_br(bb_merge, hit_args);

    b.position_at_end(bb_miss);
    Value* slow_val = miss.get(b);
    // The access may have collected, so the receiver's bits are read again.
    emit_static_fill(b, miss.receiver(b), key_index, site, /*for_write=*/false);
    std::vector<Value*> miss_args{slow_val};
    if (number) miss_args.push_back(b.build_ule(slow_val, iconst(b, kBronzeNumberMaxBits)));
    append_memo(miss_args, miss.memo_miss, b);
    b.build_br(bb_merge, miss_args);

    b.position_at_end(bb_merge);
    merge_memo(b, bb_merge, hit_args.size() - values, miss);
    if (number) set_number_flag(merge_val, merge_flag);
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
    site_branch(b, guard_or_miss(b, *this, obj, {term}, miss, bb_miss, ptr), bb_hit, bb_miss, miss.exits);

    // A hit stores into a slot the shape already has, which moves no shape;
    // the memo crosses it as it is.
    b.position_at_end(bb_hit);
    int32_t off = 0;
    Value* base = static_slot_base(b, ptr, site.slot, off);
    store_with_barrier(b, base, off, val, need_barrier, prefix);
    std::vector<Value*> hit_args;
    append_memo(hit_args, miss.memo_hit, b);
    b.build_br(bb_merge, hit_args);

    b.position_at_end(bb_miss);
    miss.set(b);
    emit_static_fill(b, miss.receiver(b), key_index, site, /*for_write=*/true);
    std::vector<Value*> miss_args;
    append_memo(miss_args, miss.memo_miss, b);
    b.build_br(bb_merge, miss_args);

    b.position_at_end(bb_merge);
    merge_memo(b, bb_merge, hit_args.size(), miss);
}

Value* PropertyLoweringHelper::lower_prop_get_guarded(Builder& b, Value* obj, const StaticSlotSite& site) {
    Value* ptr = b.build_xor(obj, iconst(b, uint64_t{BRONZE_ABI_TAG_OBJECT} << 48));
    int32_t off = 0;
    Value* base = static_slot_base(b, ptr, site.slot, off);
    return with_alias_class(b.build_load(Type::i64(), base, off), kObjectSlotAliasClass);
}

void PropertyLoweringHelper::lower_prop_set_guarded(Builder& b, Value* obj, Value* val, const StaticSlotSite& site) {
    Value* ptr = b.build_xor(obj, iconst(b, uint64_t{BRONZE_ABI_TAG_OBJECT} << 48));
    int32_t off = 0;
    Value* base = static_slot_base(b, ptr, site.slot, off);
    store_with_barrier(b, base, off, val, /*need_barrier=*/false, "");
}

Value* PropertyLoweringHelper::lower_is_shape(Builder& b, Value* obj, Value* keyed, const StaticSlotSite& site) {
    Value* ptr = nullptr;
    return static_guard(b, obj, keyed, {StaticGuardTerm{site}}, ptr);
}

// Whether `is.shape` tests anything here and a shape-guarded site is its bare
// access: the same conditions under which the site itself would take its
// static guard, so the two always agree. Where they fail `is.shape` answers
// no, and the fast copy whose sites it guards is never past it.
static bool shape_guard_lowerable(IlLowering* lowering, const BronzeInstruction& inst_ast) {
    const PropertyLoweringHelper& prop_lowering = lowering->prop_lowering();
    const StaticSlotSite site = StaticSlotSite::of(inst_ast);
    return !site.none() && prop_lowering.enable_inlined_fastpaths() && !lowering->run_once() &&
           prop_lowering.static_site_backed(site);
}

Value* load_layout_epoch(Builder& b) {
    // The TLS word `layout.epoch` reads (il_lowering_ops.cpp), as its i64.
    Module* mod = b.current_block()->parent()->parent();
    Value* tls = (mod && mod->pinned_tls_register()) ? b.build_pinned_tls_read()
                                                     : b.build_call("bronze_tls_block_addr", Type::i64(), {});
    return runtime_word(b, tls, BRONZE_TLS_PROTO_EPOCH_OFF);
}

bool has_layout_fact(const BronzeInstruction& inst_ast) {
    if (!inst_ast.shape_guarded) return false;
    return (inst_ast.op == BronzeOp::PropGet && inst_ast.operands.size() == 2) ||
           (inst_ast.op == BronzeOp::PropSet && inst_ast.operands.size() == 3);
}

// A fact's f64 is the word's bits and not its value (the IL's "exact double"
// is the interpreter's reading; here its only readers are the fact sites and
// nothing computes with it), so making one and asking one are moves between
// register classes that fold away where the two meet, and neither is a
// conversion. Either word is a small integer, so the bits are never a NaN's
// and a box of them keeps them.
static Value* layout_fact_of(Builder& b, Value* word) { return b.build_bitcast_f64_i64(word); }
Value* layout_fact_word(Builder& b, Value* fact) { return b.build_bitcast_i64_f64(fact); }

Value* layout_fact_holds(Builder& b, Value* fact) {
    return b.build_eq(load_layout_epoch(b), layout_fact_word(b, fact));
}

bool lower_shape_guarded(IlLowering* lowering, const BronzeInstruction& inst_ast, Builder& b, Value* obj, Value* val,
                         Value*& res) {
    PropertyLoweringHelper& prop_lowering = lowering->prop_lowering();
    const bool here = shape_guard_lowerable(lowering, inst_ast);
    // A layout fact: the claim's guard where the receiver stands now, and
    // the epoch read beside it (lower/layout_facts.h). Where the guard is not
    // lowered here the fact is 1, which no epoch is (epochs are even, from 2),
    // and every site holding it takes its ordinary path.
    if (inst_ast.op == BronzeOp::LayoutFact) {
        if (!here) {
            res = layout_fact_of(b, iconst(b, 1));
            return true;
        }
        Value* ok = prop_lowering.lower_is_shape(b, obj, lowering->memo_keyed(b, inst_ast.operands[0], obj),
                                                 StaticSlotSite::of(inst_ast));
        res = layout_fact_of(b, b.build_select(ok, load_layout_epoch(b), iconst(b, 1)));
        lowering->memo_keep();
        return true;
    }
    // A site holding a fact is lowered by the caller (lower_fact_site); a
    // fact is no licence for an unconditional bare access.
    if (has_layout_fact(inst_ast)) return false;
    // A guard reads and decides, so the memo answers the receiver's keyed
    // shape and stays.
    if (inst_ast.op == BronzeOp::IsShape) {
        if (!here) {
            res = b.build_iconst_i32(0);
            return true;
        }
        // Left to the guard branch behind it (il_lowering.h, "A SHAPE TEST
        // THAT LEAVES"), which reads no value of this one.
        if (lowering->defer_shape_test(inst_ast, obj)) {
            res = b.build_iconst_i32(1);
        } else {
            res = prop_lowering.lower_is_shape(b, obj, lowering->memo_keyed(b, inst_ast.operands[0], obj),
                                               StaticSlotSite::of(inst_ast));
        }
        lowering->memo_keep();
        return true;
    }
    if (!inst_ast.shape_guarded || !here) return false;
    // A load, or a store into a slot the shape already has of a Number: no
    // shape moves and no barrier is owed, so the memo stays.
    if (inst_ast.op == BronzeOp::PropGet) {
        res = prop_lowering.lower_prop_get_guarded(b, obj, StaticSlotSite::of(inst_ast));
        // Behind the `is.shape` that admitted this layout cell: a `number`
        // member is a Number.
        if (prop_lowering.layout_number(StaticSlotSite::of(inst_ast))) {
            prop_lowering.set_number_flag(res, b.build_iconst_i32(1));
        }
    } else {
        prop_lowering.lower_prop_set_guarded(b, obj, val, StaticSlotSite::of(inst_ast));
    }
    lowering->memo_keep();
    return true;
}

Value* lower_fact_site(IlLowering* lowering, Builder& b, Value* fact,
                       const std::unordered_map<uint32_t, Value*>& val_map, bool has_result,
                       const std::function<Value*(Builder&)>& bare, const std::function<Value*(Builder&)>& ordinary) {
    BasicBlock* bb_cur = b.current_block();
    const std::string prefix = "fact_site_" + std::to_string(bb_cur->parent()->next_block_id());
    BasicBlock* bb_hit = b.append_block(prefix + "_hit");
    BasicBlock* bb_miss = b.append_block(prefix + kMissName);
    BasicBlock* bb_merge = b.append_block(prefix + "_merge");
    // The shape memo crosses the site as a static site's join carries it:
    // the hit moves no shape, and the miss, which may run anything, reads
    // every memoized receiver's keyed shape again.
    const auto memo = lowering->memo_snapshot();

    b.position_at_end(bb_cur);
    b.build_br_if(layout_fact_holds(b, fact), bb_hit, bb_miss);

    b.position_at_end(bb_hit);
    std::vector<Value*> hit_args;
    Value* hit = bare(b);
    if (has_result) hit_args.push_back(hit);
    for (const auto& entry : memo) hit_args.push_back(entry.second);
    b.build_br(bb_merge, hit_args);

    // The site exactly as it is without a fact: its own guard, cache and fill.
    b.position_at_end(bb_miss);
    std::vector<Value*> miss_args;
    Value* missed = ordinary(b);
    if (has_result) miss_args.push_back(missed);
    lowering->memo_restore(memo);
    for (Value* v : lowering->memo_recompute(b, val_map)) miss_args.push_back(v);
    b.build_br(bb_merge, miss_args);

    b.position_at_end(bb_merge);
    Value* res = has_result ? b.add_block_param(bb_merge, Type::i64()) : nullptr;
    std::vector<std::pair<uint32_t, Value*>> merged;
    for (const auto& entry : memo) merged.emplace_back(entry.first, b.add_block_param(bb_merge, Type::i64()));
    lowering->memo_restore(std::move(merged));
    lowering->memo_keep();
    return res;
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

// The names whose slots may be born doubles (il.h `slotReprFields`; runtime
// slot_repr.h), as module key indices on the stack: the runtime maps each
// through the key map and keeps the names.
void IlLowering::register_slot_repr(Builder& b, Value* map_addr) {
    const std::vector<uint32_t>& fields = options_.slot_repr_fields;
    if (fields.empty()) return;
    Value* list = b.build_alloca(static_cast<uint32_t>(fields.size() * 4), 4);
    for (size_t i = 0; i < fields.size(); ++i) {
        b.build_store(Type::i32(), list, static_cast<int32_t>(i * 4), b.build_iconst_i32(static_cast<int32_t>(fields[i])));
    }
    b.build_call("bronze_register_slot_repr", Type::void_type(),
                 {list, b.build_iconst_i32(static_cast<int32_t>(fields.size())), map_addr});
}

// Whether a site's layout claim gets the static-slot guard: a named key, in
// code that runs more than once (code that runs once calls the helper, as
// every other site there does). A claim this module has no table for is a
// lowering bug, refused by name rather than quietly lowered without its guard.
static bool static_site_lowerable(IlLowering* lowering, const BronzeInstruction& inst_ast) {
    const PropertyLoweringHelper& prop_lowering = lowering->prop_lowering();
    const StaticSlotSite site = StaticSlotSite::of(inst_ast);
    if (site.none() || inst_ast.index == PropertyLoweringHelper::kNoKey) return false;
    if (!prop_lowering.enable_inlined_fastpaths() || lowering->run_once()) return false;
    if (!prop_lowering.static_site_backed(site)) {
        lowering->set_has_error(true);
        if (lowering->diag()) {
            lowering->diag()->error(SourceLocation("", inst_ast.line, inst_ast.column),
                                    "il2mir: static slot " + std::to_string(site.slot) + " of '" +
                                        std::string(prop_lowering.key_tag(inst_ast.index)) +
                                        "' names a guard table the module does not have");
        }
        return false;
    }
    return true;
}

bool static_get_lowerable(IlLowering* lowering, const BronzeInstruction& inst_ast) {
    return inst_ast.op == BronzeOp::PropGet && !inst_ast.operands.empty() && static_site_lowerable(lowering, inst_ast);
}

// A constructor's writes and the writes to an object this function made are
// adds: the property is not there yet, the guard could only miss, and the
// cache's transition arm is what answers them. A census build counts every
// write through the helper (inline_sets off).
bool static_set_lowerable(IlLowering* lowering, const BronzeInstruction& inst_ast) {
    return inst_ast.op == BronzeOp::PropSet && inst_ast.operands.size() >= 2 &&
           lowering->prop_lowering().inline_sets() && !lowering->in_class_constructor() &&
           !lowering->is_fresh_object(inst_ast.operands[0]) && static_site_lowerable(lowering, inst_ast);
}

} // namespace il2mir
