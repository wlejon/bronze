#pragma once

#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <functional>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "il_ast.h"

namespace il2mir {

class SpecSiteEmitter;

// One inline-cache site: its way 0 is at `base + off`. Inline paths load
// its words at immediate offsets from `base` (the function's one table
// address); only a helper call needs the site's own address.
struct IcSite {
    Value* base = nullptr;
    int32_t off = 0;
    // The site's number: its packed way-0 row (bronze_abi_layout.h, "the HOT
    // array") is at index * BRONZE_ABI_IC_HOT_ENTRY_SIZE in the hot table.
    uint32_t index = 0;
    explicit operator bool() const noexcept { return base != nullptr; }
};

class PropertyLoweringHelper {
public:
    // A key operand that names no key constant.
    static constexpr uint32_t kNoKey = 0xFFFFFFFFu;

    [[nodiscard]] const std::string& key_map_sym() const noexcept { return key_map_sym_; }
    void set_key_map_sym(std::string sym) { key_map_sym_ = std::move(sym); }

    // The module's inline-cache table: `site_count` sites of
    // kBronzeIcSiteSize bytes at the data symbol `sym`. Zero sites (the
    // default) means no table, and every site pointer stays null.
    void set_ic_table(std::string sym, uint32_t site_count) {
        ic_table_sym_ = std::move(sym);
        ic_site_count_ = site_count;
    }
    [[nodiscard]] uint32_t ic_site_count() const noexcept { return ic_site_count_; }

    // PROVEN FACTS (il_speculation.h, resolve_fact_markers), in an image:
    // `prove_plain` marks the block the builder is in as one only a plain
    // object `v` reaches, which holds for the rest of the function, a
    // value's tag and an object's heap kind being fixed at its creation.
    // `proven_plain` is an i32 for a later receiver test of `v`, 1 where one
    // of the marked blocks dominates it in the function as finally built
    // (the speculated copy, whose misses left; rarely the twin, entered at
    // its resume blocks) and 0 elsewhere, or null with nothing marked.
    void prove_plain(Builder& b, Value* v);
    Value* proven_plain(Builder& b, Value* v);
    // Per function: forgets the proofs of the one before.
    void clear_facts() {
        plain_proofs_.clear();
        next_proof_ = 0;
    }

    // SpecSiteEmitter::site_branch, for the sites lowered here.
    void site_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, bool exits,
                     const std::vector<Value*>& fast_args = {});
    // SLOW ARMS (il_speculation.h): the instruction being lowered's slow
    // site, set by IlLowering for every instruction. `slow_branch` is
    // site_branch to a slow arm, `slow_arm` starts one (its counter in a
    // build that may be harvested): every slow block of an inline fast path
    // whose generic code runs the instruction from its start, with nothing
    // done yet that it would do again.
    void set_slow_sites(std::vector<uint32_t> sites) { slow_sites_ = std::move(sites); }
    void slow_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow,
                     const std::vector<Value*>& fast_args = {}) {
        site_branch(b, hit, fast, slow, /*exits=*/false, fast_args);
    }
    void slow_arm(Builder& b) {
        if (!slow_count_fn_) return;
        for (const uint32_t site : slow_sites_) slow_count_fn_(b, site);
    }
    void set_slow_count_fn(std::function<void(Builder&, uint32_t)> fn) { slow_count_fn_ = std::move(fn); }
    // ARMS (il_speculation.h): the alternatives of the instruction's inline
    // path, each an index after its slow site's (below kSlowSiteArms).
    // `arm_begin` starts one, marking it run in a build that may be
    // harvested.
    void set_arm_mark_fn(std::function<void(Builder&, uint32_t)> mark) { arm_mark_fn_ = std::move(mark); }
    void arm_begin(Builder& b, uint32_t arm) {
        if (arm_mark_fn_ && slow_sites_.size() == 1) arm_mark_fn_(b, slow_sites_[0] + arm);
    }
    // Whether arm_begin marks here: a build that may be harvested, at an
    // instruction with a site of its own.
    [[nodiscard]] bool arms_marked() const noexcept { return arm_mark_fn_ && slow_sites_.size() == 1; }
    // SHAPE TURNS (il/feedback_site.h kShapeTurnArm), in a build made to be
    // harvested, in front of a named read or write with a cache: one more
    // where `obj`'s keyed shape is not the one the site's last receiver
    // had, and that shape kept. No branch: the count takes the compare's
    // bit. `fn(b, slow site, keyed shape)`.
    void set_shape_turn_fn(std::function<void(Builder&, uint32_t, Value*)> fn) { shape_turn_fn_ = std::move(fn); }
    void count_shape_turn(Builder& b, Value* obj, const IcSite& site);

    // With per-thread module data (TranslatorOptions::per_thread_module_data)
    // the table's address is the symbol plus the calling thread's delta,
    // which the owner of the function being lowered supplies; unset, the
    // symbol's own address is the table.
    void set_module_delta_fn(std::function<Value*(Builder&)> fn) { module_delta_fn_ = std::move(fn); }
    // The function being lowered hands in its table address, computed once
    // at its entry; unset, every site computes its own.
    void set_table_base_fn(std::function<Value*(Builder&)> fn) { table_base_fn_ = std::move(fn); }

    // Site `ic_index`, or a null site when the index names none in this
    // module's table (kNoIcIndex, or a module compiled without one).
    IcSite ic_site_ref(Builder& b, uint32_t ic_index);
    // The address of the site's way 0: what the bronze helpers take as their
    // BRONZE_ABI_MU64 operand.
    Value* site_ptr(Builder& b, const IcSite& site);
    // ic_site_ref's address, or null.
    Value* ic_site(Builder& b, uint32_t ic_index);

    // The runtime's id for key constant `key_index` (read through the
    // module's key map), or kNoKey as itself.
    Value* runtime_key(Builder& b, uint32_t key_index);

    // `bronze_prop_get` on key constant `key_index`, through the site (a
    // null site: no cache).
    Value* lower_prop_get(Builder& b, Value* obj, uint32_t key_index, const IcSite& site);

    // A property read's inline-cache site: the receiver's shape word against
    // the site's way 0 and its slot word below the inline-slot count
    // (bronze_abi.h, BRONZE_ABI_IC_SLOTWORD_OFFSET), one slot load on a hit
    // and `bronze_prop_get` on a miss. Requires a real site.
    Value* lower_prop_get_mono(Builder& b, Value* obj, uint32_t key_index, const IcSite& site);

    // `bronze_prop_set`; `imm` nonzero is a strict-mode write.
    void lower_prop_set(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm, Value* ic_entry);

    // A property write's inline-cache site: a plain object whose shape is
    // the site's way 0, whose slot word names an own data slot with no flag
    // (not an accessor; an f64 slot only for a Number), stores the value
    // there, inline or overflow, with the write barrier for a reference; the
    // helper on a miss. Requires a real site.
    void lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm,
                             const IcSite& site, bool val_is_primitive = false);

    // The speculation sites' emitter (il_speculation.h); unset, plain branches.
    void set_spec(SpecSiteEmitter* spec) noexcept { spec_ = spec; }
    // Whether writes get lower_prop_set_mono (off while a census counts
    // every write through the helper).
    void set_inline_sets(bool on) noexcept { inline_sets_ = on; }
    [[nodiscard]] bool inline_sets() const noexcept { return inline_sets_; }
    // Whether the module runs in this process (the tiered pipeline), so its
    // code may hold this process's addresses.
    void set_in_process(bool on) noexcept { in_process_ = on; }
    [[nodiscard]] bool in_process() const noexcept { return in_process_; }
    // The module's key constants, which name a property site's tag.
    void set_key_names(const std::vector<std::string>* names) noexcept { key_names_ = names; }
    [[nodiscard]] std::string_view key_tag(uint32_t key_index) const noexcept {
        return key_names_ && key_index < key_names_->size() ? std::string_view((*key_names_)[key_index])
                                                            : std::string_view();
    }

    // `bronze_elem_get` / `bronze_elem_set` (il_elem_lowering.cpp).
    Value* lower_elem_get(Builder& b, Value* obj, Value* index);
    void lower_elem_set(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict);

    // `is.dense_array`: whether `obj` is an Array with an elements block and
    // `max_index < length`.
    Value* lower_is_dense_array(Builder& b, Value* obj, uint32_t max_index);
    // `is.typed_array`: whether `obj` is a TypedArray of `elem_kind`, as an i32
    // (and, with kTypedArrayPristineLength in `imm`, whether its chain is
    // pristine as typed_array_chain_pristine says).
    Value* lower_is_typed_array(Builder& b, Value* obj, int64_t imm);
    // `is.plain_array`: an Array whose props word holds no object.
    Value* lower_is_plain_array(Builder& b, Value* obj);
    // Whether the view at `ptr`, of element kind `kind` (an i64), answers
    // `length` from its header the way the runtime's read would
    // (rtTypedArrayChainPristine). An i32.
    Value* typed_array_chain_pristine(Builder& b, Value* ptr, Value* kind);

    bool is_known_non_ref(Value* val) const;
    Value* box_number(Builder& b, Value* f64_val);

    void lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure);

    // A value's object payload and shape word (every object kind has one),
    // and whether it is an object at all; a non-object reads its shape word
    // off a mapped block, and `is_object` false refuses every way.
    void receiver_shape(Builder& b, Value* obj, Value*& is_object, Value*& ptr, Value*& shape);
    // The base a slot is read at: base + slot_word * 8 +
    // BRONZE_ABI_OBJ_SLOTS_OFFSET, inline or overflow.
    Value* slot_base(Builder& b, Value* ptr, Value* slot_word);
    // Slot `slot_word` of the plain object at `ptr` (inline or overflow).
    Value* load_slot(Builder& b, Value* ptr, Value* slot_word);
    // load_slot as a branch on the slot word (inline or overflow), leaving
    // the builder in the join: for a point that may be split.
    Value* load_slot_br(Builder& b, Value* ptr, Value* slot_word);

private:
    // A site's way-0 shape and slot words, from its packed row
    // (bronze_abi_layout.h, "the HOT array"), addressed off the site's own
    // base: what every way-0 guard compares and loads.
    int32_t hot_row_off(const IcSite& site) const;
    Value* way0_shape(Builder& b, const IcSite& site);
    Value* way0_slot_word(Builder& b, const IcSite& site);
    // Monomorphic way-0 guard on a pre-computed receiver shape: checks that
    // `is_object` holds, `shape` matches way 0 of `site`, and its slot word is an
    // own data slot. A write passes `number_store`, its value is a Number,
    // and then also hits a double slot's entry (whose slot word it masks).
    void emit_mono_slot_guard(Builder& b, Value* shape, Value* is_object, const IcSite& site,
                              Value*& hit, Value*& slot_word, Value* number_store = nullptr);

    SpecSiteEmitter* spec_ = nullptr;
    bool inline_sets_ = true;
    bool in_process_ = false;
    const std::vector<std::string>* key_names_ = nullptr;
    std::string key_map_sym_ = "__bronze_key_map";
    std::string ic_table_sym_ = "__bronze_ic_table";
    uint32_t ic_site_count_ = 0;
    std::unordered_map<Value*, std::vector<uint32_t>> plain_proofs_;
    uint32_t next_proof_ = 0;
    std::function<Value*(Builder&)> module_delta_fn_;
    std::function<Value*(Builder&)> table_base_fn_;
    std::function<void(Builder&, uint32_t)> slow_count_fn_;
    std::function<void(Builder&, uint32_t)> arm_mark_fn_;
    std::vector<uint32_t> slow_sites_;
    std::function<void(Builder&, uint32_t, Value*)> shape_turn_fn_;
};

} // namespace il2mir
