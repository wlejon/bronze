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
    Value* lower_prop_get_mono(Builder& b, Value* obj, uint32_t key_index, const IcSite& site);

    // `bronze_prop_set`; `imm` nonzero is a strict-mode write.
    void lower_prop_set(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm, Value* ic_entry);

    // A property write with the monomorphic inline store in front of
    // `bronze_prop_set`: a plain object whose shape is the site's way 0,
    // whose slot word names an own data slot with no flag (not an accessor,
    // not an f64 slot), stores the value there, inline or overflow, with the
    // write barrier for a reference. Requires a real site.
    void lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm,
                             const IcSite& site);

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

    void lower_elem_set(
        Builder& b,
        Value* obj,
        Value* index,
        Value* val,
        uint32_t strict
    );

    // The helper calls alone, for code that runs once.
    Value* lower_elem_get_call(Builder& b, Value* obj, Value* index);
    void lower_elem_set_call(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict);

    void lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure);

    // A plain object's payload and shape word, and whether it is one; a
    // non-object reads a mapped block whose words refuse every way.
    void receiver_shape(Builder& b, Value* obj, Value*& plain, Value*& ptr, Value*& shape);

private:
    // The shared test of the two mono paths: `hit`, the receiver's payload
    // and the site's slot word.
    void mono_hit(Builder& b, Value* obj, const IcSite& site, Value*& hit, Value*& ptr, Value*& slot_word);
    // The read's poly test: the receiver's shape against the site's ways
    // 1..N-1, with the matching way's slot word.
    void other_ways_hit(Builder& b, Value* plain, Value* shape, const IcSite& site, Value*& hit,
                        Value*& slot_word);
    // The base a slot is read at: base + slot_word * 8 +
    // BRONZE_ABI_OBJ_SLOTS_OFFSET, inline or overflow.
    Value* slot_base(Builder& b, Value* ptr, Value* slot_word);
    SpecSiteEmitter* spec_ = nullptr;
    bool inline_sets_ = true;
    bool feedback_driven_ = false;
    bool in_process_ = false;
    bool small_forms_ = true;
    const std::vector<std::string>* key_names_ = nullptr;
    bool enable_inlined_fastpaths_ = true;
    std::string key_map_sym_ = "__bronze_key_map";
    std::string ic_table_sym_ = "__bronze_ic_table";
    uint32_t ic_site_count_ = 0;
    std::function<Value*(Builder&)> module_delta_fn_;
    std::function<Value*(Builder&)> table_base_fn_;
};

} // namespace il2mir
