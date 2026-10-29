#pragma once

#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <functional>
#include <string_view>
#include <string>
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
    explicit operator bool() const noexcept { return base != nullptr; }
};

// The out-of-line scan of a read's other ways (bronze_abi.h), which tier 2
// expands back inline (il_ic_stub.h).
inline constexpr const char* kPolyScanHelper = "bronze_prop_poly_scan";

// The module's out-of-line property sites (il_lowering_direct.cpp): what a
// read or write that would have called bronze_prop_get / bronze_prop_set
// calls instead in an image build. They answer from the site's cache what the
// helper's own fast path answers (an own data slot, an absent key, a
// prototype's data slot or accessor) and call the helper for the rest, with
// the same operands.
inline constexpr const char* kPropGetStub = "__bronze_prop_get_ic";
inline constexpr const char* kPropSetStub = "__bronze_prop_set_ic";

// A site whose class layout proved its key's instance slot (il.h
// `staticSlot`), with the guard that licenses the constant offset: the
// identity form's module cell, or the family form's class-id range.
struct StaticSlotSite {
    uint32_t slot = BronzeInstruction::kNoStaticSlot;
    uint32_t cell = 0;
    uint32_t family_lo = BronzeInstruction::kNoFamily;
    uint32_t family_span = 0;

    static StaticSlotSite of(const BronzeInstruction& inst) {
        return {inst.static_slot, inst.static_cell, inst.family_lo, inst.family_span};
    }
    bool none() const { return slot == BronzeInstruction::kNoStaticSlot; }
    bool family() const { return family_lo != BronzeInstruction::kNoFamily; }
};

// One site's part of a static guard: its layout claim and, for a write, the
// value it will store (whose representation the guard must accept).
struct StaticGuardTerm {
    StaticSlotSite site;
    Value* store = nullptr;
    bool store_is_number = false;
};

// What a static site does when its guard misses: the ordinary sequence for a
// read (`get`) or a write (`set`), and the receiver's bits read afresh after
// it (`receiver`), for the fill.
struct StaticMiss {
    std::function<Value*(Builder&)> get;
    std::function<void(Builder&)> set;
    std::function<Value*(Builder&)> receiver;
};

class PropertyLoweringHelper {
public:
    // A key operand that names no key constant.
    static constexpr uint32_t kNoKey = 0xFFFFFFFFu;

    explicit PropertyLoweringHelper(bool enable_inlined_fastpaths = true)
        : enable_inlined_fastpaths_(enable_inlined_fastpaths) {}

    [[nodiscard]] bool enable_inlined_fastpaths() const noexcept { return enable_inlined_fastpaths_; }
    void set_enable_inlined_fastpaths(bool enable) noexcept { enable_inlined_fastpaths_ = enable; }

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

    // The static-slot guards' tables (TranslatorOptions::static_site_count):
    // `cell_count` i64 cells at `cells_sym`, and the family base word (then a
    // zero word) at `family_sym`, which a family guard needs `family_count`
    // registered classes to have.
    void set_static_tables(std::string cells_sym, uint32_t cell_count, std::string family_sym,
                           uint32_t family_count) {
        static_cells_sym_ = std::move(cells_sym);
        static_cell_count_ = cell_count;
        family_base_sym_ = std::move(family_sym);
        family_count_ = family_count;
    }
    // Whether this module has the table `site`'s guard reads. A site it has
    // none for is a lowering bug the caller diagnoses, never a quiet skip.
    [[nodiscard]] bool static_site_backed(const StaticSlotSite& site) const noexcept {
        if (site.none()) return false;
        return site.family() ? family_count_ > 0 && site.family_lo < family_count_
                             : site.cell < static_cell_count_;
    }

    // The static cells' table on this thread, and whether it holds the
    // `n` cells from `cell` (an accessor site's pair, il_accessor_direct.h).
    Value* static_cells_addr(Builder& b) { return data_addr(b, static_cells_sym_); }
    [[nodiscard]] bool static_cells_cover(uint32_t cell, uint32_t n) const noexcept {
        return static_cast<uint64_t>(cell) + n <= static_cell_count_;
    }

    // THE STATIC-SLOT GUARD (il_static_slot.cpp) for every term on the one
    // receiver `obj`: whether `obj` is a plain object whose shape each
    // term's site admits — and, for a term that stores, whose slot takes
    // the value bare — as one i1 before one branch. `ptr` is the receiver's
    // payload, meaningful only where the i1 holds.
    Value* static_guard(Builder& b, Value* obj, const std::vector<StaticGuardTerm>& terms, Value*& ptr);
    // Where slot `slot` of the plain object at `ptr` is: a base and the
    // constant `off` from it, inline or overflow chosen here.
    Value* static_slot_base(Builder& b, Value* ptr, uint32_t slot, int32_t& off);
    // The slot store, then the barrier when `need_barrier` (a value not known
    // to be primitive); leaves the builder after both.
    void store_with_barrier(Builder& b, Value* base, int32_t off, Value* val, bool need_barrier,
                            const std::string& prefix);
    // The one-shot fill a static site's miss runs after the ordinary access:
    // the identity cell's publish while it is still zero, or the shape's
    // family stamp while that is zero. `obj` must be bits read after the
    // access, which may have collected. Leaves the builder after it.
    void emit_static_fill(Builder& b, Value* obj, uint32_t key_index, const StaticSlotSite& site, bool for_write);
    // One static read: the guard, a constant-offset load on a hit, and on a
    // miss the site's ordinary sequence and then the fill.
    Value* lower_prop_get_static(Builder& b, Value* obj, uint32_t key_index, const StaticSlotSite& site,
                                 const StaticMiss& miss);
    // One static write, likewise; `need_barrier` false for a primitive value.
    void lower_prop_set_static(Builder& b, Value* obj, uint32_t key_index, Value* val, const StaticSlotSite& site,
                               bool val_is_number, bool need_barrier, const StaticMiss& miss);

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

    // `bronze_prop_get` on key constant `key_index`, through the site
    // `ic_entry` (null: no site).
    Value* lower_prop_get(Builder& b, Value* obj, uint32_t key_index, Value* ic_entry);

    // A bronze property read with the monomorphic inline hit in front of the
    // helper: the receiver's shape word against the site's way 0 and its
    // slot word below the inline-slot count (bronze_abi.h,
    // BRONZE_ABI_IC_SLOTWORD_OFFSET), one slot load on a hit and
    // `bronze_prop_get` on a miss. `key_index` is the module's key constant.
    // Requires a real site.
    Value* lower_prop_get_mono(Builder& b, Value* obj, uint32_t key_index, const IcSite& site,
                               bool expect_poly = false);

    // `bronze_prop_set`; `imm` nonzero is a strict-mode write.
    void lower_prop_set(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm, Value* ic_entry);

    // A property write with the monomorphic inline store in front of
    // `bronze_prop_set`: a plain object whose shape is the site's way 0,
    // whose slot word names an own data slot with no flag (not an accessor,
    // not an f64 slot), stores the value there, inline or overflow, with the
    // write barrier for a reference. Requires a real site.
    // `with_transition` adds, on the miss, the inline shape-transition store
    // (emit_transition_arm) ahead of the helper.
    void lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm,
                             const IcSite& site, bool val_is_primitive = false, bool with_transition = false);

    // Tier 2's expansion of a kPolyScanHelper call: the scan inline, as
    // straight-line code at the builder's position. `hit` is whether a way
    // described the receiver, and on a hit load_slot(ptr, slot_word) reads
    // the property. The counter operand is incremented as the helper would.
    void expand_poly_scan(Builder& b, Instruction* call, Value*& hit, Value*& ptr, Value*& slot_word);
    // Slot `slot_word` of the plain object at `ptr` (inline or overflow).
    Value* load_slot(Builder& b, Value* ptr, Value* slot_word);

    // The speculation sites' emitter (il_speculation.h); unset, plain branches.
    void set_spec(SpecSiteEmitter* spec) noexcept { spec_ = spec; }
    // Whether writes get lower_prop_set_mono (off while a census counts
    // every write through the helper).
    void set_inline_sets(bool on) noexcept { inline_sets_ = on; }
    [[nodiscard]] bool inline_sets() const noexcept { return inline_sets_; }
    // Whether the program counts its sites' misses (TranslatorOptions::
    // spec_feedback): every keyed site then gets the inline path.
    void set_feedback_driven(bool on) noexcept { feedback_driven_ = on; }
    [[nodiscard]] bool feedback_driven() const noexcept { return feedback_driven_; }
    // Whether the module runs in this process (the tiered pipeline), so its
    // code may hold this process's addresses; an AOT lowering driven by a
    // profile is feedback-driven but not in-process.
    void set_in_process(bool on) noexcept { in_process_ = on; }
    [[nodiscard]] bool in_process() const noexcept { return in_process_; }
    // Whether a feedback-driven module that runs here takes the small forms
    // (the scan and the misses out of line); off, BRONZE_IC_FULL=1.
    void set_small_forms(bool on) noexcept { small_forms_ = on; }
    // Whether the helper calls of lower_prop_get / lower_prop_set (and the
    // misses of the inline paths) go through kPropGetStub / kPropSetStub,
    // which the module then defines (emit_direct_stubs).
    void set_ic_stubs(bool on) noexcept { ic_stubs_ = on; }
    [[nodiscard]] bool ic_stubs() const noexcept { return ic_stubs_; }
    // The module's key constants, which name a property site in a profile.
    void set_key_names(const std::vector<std::string>* names) noexcept { key_names_ = names; }
    [[nodiscard]] std::string_view key_tag(uint32_t key_index) const noexcept {
        return key_names_ && key_index < key_names_->size() ? std::string_view((*key_names_)[key_index])
                                                            : std::string_view();
    }

    Value* lower_elem_get(
        Builder& b,
        Value* obj,
        Value* index
    );

    Value* lower_elem_get_typed(
        Builder& b,
        Value* obj,
        Value* index,
        int64_t elem_kind,
        Value* preloaded_data = nullptr,
        Value* preloaded_length = nullptr
    );

    void lower_elem_set(
        Builder& b,
        Value* obj,
        Value* index,
        Value* val,
        uint32_t strict
    );

    void lower_elem_set_typed(
        Builder& b,
        Value* obj,
        Value* index,
        Value* val_f64,
        int64_t elem_kind,
        uint32_t strict,
        Value* preloaded_data = nullptr,
        Value* preloaded_length = nullptr
    );

    // The helper calls alone, for code that runs once.
    Value* lower_elem_get_call(Builder& b, Value* obj, Value* index);
    void lower_elem_set_call(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict);

    // `is.dense_array`: whether `obj` is an Array with an elements block and
    // `max_index < length`.
    Value* lower_is_dense_array(Builder& b, Value* obj, uint32_t max_index);
    // `is.typed_array`: whether `obj` is a TypedArray of `elem_kind`, as an i32
    // (and, with kTypedArrayPristineLength in `imm`, whether its chain is
    // pristine as typed_array_chain_pristine says).
    Value* lower_is_typed_array(Builder& b, Value* obj, int64_t imm);
    // Whether the view at `ptr`, of element kind `kind` (an i64), answers
    // `length` from its header the way the runtime's read would
    // (rtTypedArrayChainPristine): its kind's intrinsic instance shape, and
    // the family verified pristine at the current prototype-mutation epoch
    // (TLS ta_length_tbl). An i32.
    Value* typed_array_chain_pristine(Builder& b, Value* ptr, Value* kind);

    // Element address and load/store on an Array already proven dense in range
    // of `index`.
    void load_dense_array_cursor(Builder& b, Value* obj, Value*& out_block, Value*& out_head);
    Value* dense_array_slot_addr(Builder& b, Value* block, Value* head, uint32_t index);
    Value* load_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index, bool check_hole);
    void store_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index, Value* val);
    bool is_known_non_ref(Value* val) const;

    // Guard for an Array read/write run over constant indices `0..max_index`:
    // sets `out_ok`, `out_block` and `out_head`.
    void emit_dense_array_guard(Builder& b, Value* obj, uint32_t max_index, bool writable,
                                Value*& out_ok, Value*& out_block, Value*& out_head);

    // A constant-index property read or write (`o[0]`, `o[15] = v`): an inline
    // Array element access in front of `bronze_prop_get` / `bronze_prop_set`.
    Value* lower_prop_get_index(Builder& b, Value* obj, uint32_t key_index, uint32_t elem_index,
                                const IcSite& site, bool check_hole = true);
    void lower_prop_set_index(Builder& b, Value* obj, uint32_t key_index, uint32_t elem_index,
                              Value* val, uint32_t strict, const IcSite& site,
                              bool val_is_primitive = false);

    // `obj.length` with inline Array and typed-array arms (each header's
    // length) in front of the site's way-0 guard (`mono`) or the helper.
    Value* lower_prop_get_length(Builder& b, Value* obj, uint32_t key_index, const IcSite& site, bool mono,
                                 bool is_family);

    void lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure);

    // A plain object's payload and shape word, and whether it is one; a
    // non-object reads a mapped block whose words refuse every way.
    // `base`, when asked for, is the address the header and shape were read
    // at: the payload, or the mapped block a non-object reads instead.
    void receiver_shape(Builder& b, Value* obj, Value*& plain, Value*& ptr, Value*& shape, Value** base = nullptr);
    // The base a slot is read at: base + slot_word * 8 +
    // BRONZE_ABI_OBJ_SLOTS_OFFSET, inline or overflow.
    Value* slot_base(Builder& b, Value* ptr, Value* slot_word);
    void emit_transition_arm(Builder& b, Value* ptr, Value* shape, Value* plain, const IcSite& site, Value* val,
                             bool need_barrier, BasicBlock* done, const std::string& prefix);
    // Monomorphic way-0 guard on a pre-computed receiver shape: checks that
    // `plain` holds, `shape` matches way 0 of `site`, and its slot word is an
    // own data slot.
    void emit_mono_slot_guard(Builder& b, Value* shape, Value* plain, const IcSite& site,
                              Value*& hit, Value*& slot_word);

    // Helpers for batched element-offset runs (il_lowering_elem_run.cpp).
    void emit_elem_index_guard(Builder& b, Value* index, Value*& out_i64, Value*& out_ok);
    Value* unbox_known_number(Builder& b, Value* val, Value*& out_ok);
    Value* box_number(Builder& b, Value* f64_val);
    void object_payload(Builder& b, Value* obj, Value*& out_ptr, Value*& out_is_obj);
    Value* load_typed_array_data(Builder& b, Value* ptr);

private:
    // The shared test of the two mono paths: `hit`, the receiver's payload
    // and the site's slot word.
    void mono_hit(Builder& b, Value* obj, const IcSite& site, Value*& hit, Value*& ptr, Value*& slot_word);
    // The read's poly test: the receiver's shape against the site's ways
    // 1..N-1, with the matching way's slot word.
    void other_ways_hit(Builder& b, Value* plain, Value* shape, const IcSite& site, Value*& hit,
                        Value*& slot_word);
    SpecSiteEmitter* spec_ = nullptr;
    bool inline_sets_ = true;
    bool feedback_driven_ = false;
    bool in_process_ = false;
    bool small_forms_ = true;
    bool ic_stubs_ = false;
    const std::vector<std::string>* key_names_ = nullptr;
    bool enable_inlined_fastpaths_ = true;
    std::string key_map_sym_ = "__bronze_key_map";
    std::string ic_table_sym_ = "__bronze_ic_table";
    uint32_t ic_site_count_ = 0;
    std::string static_cells_sym_;
    uint32_t static_cell_count_ = 0;
    std::string family_base_sym_;
    uint32_t family_count_ = 0;
    // A writable table's address on this thread (module_delta_fn_).
    Value* data_addr(Builder& b, const std::string& sym);
    std::function<Value*(Builder&)> module_delta_fn_;
    std::function<Value*(Builder&)> table_base_fn_;
    struct LastTypedGet {
        BasicBlock* merge_bb = nullptr;
        BasicBlock* load_bb = nullptr;
        Value* result = nullptr;
        Value* loaded_val = nullptr;
        Value* data = nullptr;
        Value* idx_i64 = nullptr;
        Value* obj = nullptr;
        Value* index = nullptr;
        int64_t elem_kind = -1;
    } last_ta_get_;
};

} // namespace il2mir
