#pragma once

#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <functional>
#include <string_view>
#include <string>
#include <cstdint>
#include "il_ast.h"

namespace il2mir {

class SpecSiteEmitter;

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

    // The address of site `ic_index`'s way 0 — what the bronze helpers take
    // as their BRONZE_ABI_MU64 operand — or null when the index names no site
    // in this module's table (kNoIcIndex, or a module compiled without one).
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
    Value* lower_prop_get_mono(
        Builder& b,
        Value* obj,
        uint32_t key_index,
        Value* ic_entry
    );

    // `bronze_prop_set`; `imm` nonzero is a strict-mode write.
    void lower_prop_set(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm, Value* ic_entry);

    // A property write with the monomorphic inline store in front of
    // `bronze_prop_set`: a plain object whose shape is the site's way 0,
    // whose slot word names an own data slot with no flag (not an accessor,
    // not an f64 slot), stores the value there, inline or overflow, with the
    // write barrier for a reference. Requires a real site.
    void lower_prop_set_mono(Builder& b, Value* obj, uint32_t key_index, Value* val, uint32_t imm,
                             Value* ic_entry);

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
        uint32_t ic_slot
    );

    void lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure);

private:
    // The shared test of the two mono paths: `hit`, the receiver's payload
    // and the site's slot word.
    void mono_hit(Builder& b, Value* obj, Value* ic_entry, Value*& hit, Value*& ptr, Value*& slot_word);
    // The base a slot is read at: base + slot_word * 8 +
    // BRONZE_ABI_OBJ_SLOTS_OFFSET, inline or overflow.
    Value* slot_base(Builder& b, Value* ptr, Value* slot_word);
    SpecSiteEmitter* spec_ = nullptr;
    bool inline_sets_ = true;
    bool feedback_driven_ = false;
    bool enable_inlined_fastpaths_ = true;
    std::string key_map_sym_ = "__bronze_key_map";
    std::string ic_table_sym_ = "__bronze_ic_table";
    uint32_t ic_site_count_ = 0;
    std::function<Value*(Builder&)> module_delta_fn_;
};

} // namespace il2mir
