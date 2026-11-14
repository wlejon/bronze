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
    // Whether way-0 guards read that row. A METHOD-call site read as a
    // property (a speculated builtin's method read) is not: its way 0 is
    // rewritten in method form by the call's latches, which keep no row, so
    // its guards read the site's own first two words, which have a row's
    // layout (and a read stub is handed the site as its row).
    bool packed = true;
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
// kPropSetStub is the write's way-0 answers alone (il_prop_set_fast.cpp: an
// own data slot, an add into an inline slot); the other ways, the table, a
// setter and the helper are kPropSetSlowStub's, with the site's operands.
inline constexpr const char* kPropSetSlowStub = "__bronze_prop_set_slow";
// kPropGetStub is the read's hit paths alone (il_prop_get_fast.cpp: own and
// prototype data slots and absent keys, from any way or the megamorphic
// table), written for a leaf's registers; everything else (a getter, a
// receiver no way names, the helper) it hands to kPropGetSlowStub, which
// answers the whole read with the site's operands.
inline constexpr const char* kPropGetSlowStub = "__bronze_prop_get_slow";
// kPropGetStub that also moves a megamorphic-table hit to the front of the
// site's ways (il_prop_get_fast.cpp): the miss of an inline way chain, whose
// profile saw at most four receiver shapes, so a way the site filled with a
// shape that stopped arriving is retaken by one that did.
inline constexpr const char* kPropGetFrontStub = "__bronze_prop_get_ic_front";

// A site whose class layout proved its key's instance slot (il.h
// `staticSlot`), with the guard that licenses the constant offset: the
// identity form's module cell, or the family form's class-id range.
struct StaticSlotSite {
    uint32_t slot = BronzeInstruction::kNoStaticSlot;
    uint32_t cell = 0;
    uint32_t family_lo = BronzeInstruction::kNoFamily;
    uint32_t family_span = 0;
    // An identity cell that may latch an object under construction (il.h
    // `staticRegrow`): its fill re-latches a grown shape.
    bool regrow = false;

    static StaticSlotSite of(const BronzeInstruction& inst) {
        return {inst.static_slot, inst.static_cell, inst.family_lo, inst.family_span, inst.static_regrow};
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
    // A write, whether or not `store` is given (a value the caller knows is
    // a Number may come without it).
    bool stores = false;
};

// What a static site does when its guard misses: the ordinary sequence for a
// read (`get`) or a write (`set`), and the receiver's bits read afresh after
// it (`receiver`), for the fill.
//
// `keyed` is the receiver's KEYED SHAPE (PropertyLoweringHelper::keyed_shape),
// which the guard compares and the caller may have from an earlier site on
// the same receiver (il_shape_memo.cpp). The three memo hooks carry the
// shape memo across the site's join: what the hit edge passes on
// (`memo_hit`), what the miss edge passes after recomputing it once the
// ordinary sequence ran (`memo_miss`), and the join's parameters that stand
// for them past the site (`memo_merged`). Unset, the site has no memo to
// carry.
struct StaticMiss {
    std::function<Value*(Builder&)> get;
    std::function<void(Builder&)> set;
    std::function<Value*(Builder&)> receiver;
    Value* keyed = nullptr;
    std::function<std::vector<Value*>(Builder&)> memo_hit;
    std::function<std::vector<Value*>(Builder&)> memo_miss;
    std::function<void(const std::vector<Value*>&)> memo_merged;
    // Set when `keyed` is null: the site computes the receiver's shape on
    // its own plain-only path (plain_shape_or_miss) and hands it here, for
    // the memo, before it reads any memo hook.
    std::function<void(Value*)> memo_fresh;
    // The receiver's family id where the memo holds it, and where a guard
    // that read it hands it to the memo (before any memo hook is read).
    Value* family = nullptr;
    std::function<void(Value*)> memo_family;
    // The site's miss leaves the function (il_speculation.h, "IMAGE SITES"):
    // its claim is one the profile did not see miss.
    bool exits = false;
};

// The shape memo (il_shape_memo.cpp) carried across an inline-cache read
// (lower_prop_get_mono, lower_prop_get_absent), as a static site carries it
// (StaticMiss): `keyed` is the receiver's keyed shape, the memo's or computed
// and remembered there, which the site's way 0 is compared with; `hit` is
// what every edge into the read's join that ran no call passes, `miss` what
// an edge after the helper's call passes (each receiver's shape read again),
// and `merged` takes the join's parameters that stand for them past it.
struct MemoCarry {
    std::function<Value*(Builder&)> keyed;
    std::function<std::vector<Value*>(Builder&)> hit;
    std::function<std::vector<Value*>(Builder&)> miss;
    std::function<void(const std::vector<Value*>&)> merged;
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
    // The module's METHOD-CALL sites, sorted (TranslatorOptions::
    // method_ic_sites): a site's rank in it is its method row's
    // (bronze_abi_layout.h, "the METHOD ROWS").
    void set_method_sites(const std::vector<uint32_t>& sorted_sites) {
        method_rank_.clear();
        for (uint32_t i = 0; i < sorted_sites.size(); ++i) method_rank_.emplace(sorted_sites[i], i);
    }

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
    // The layout cells (TranslatorOptions::layout_cells): a site whose cell
    // is one publishes through emit_static_fill's layout form.
    void set_layout_cells(const std::unordered_map<uint32_t, LayoutCell>* cells) { layout_cells_ = cells; }
    // Whether `site`'s cell is a layout cell whose member at `site.slot` is a
    // `number` one (il.h `LayoutCellEntry`): the runtime publishes it only on
    // a shape that calls the slot a double, so behind its guard the slot
    // holds a Number.
    [[nodiscard]] bool layout_number(const StaticSlotSite& site) const;
    // NUMBER FLAGS. A read of a `number` member is a Number on every edge on
    // which its cell's guard held; its join carries an i32 that says, per
    // edge, whether the value is a Number (1 on the hit, the test on the
    // miss), and `is.number` of the read's result is that i32. Where the
    // site's miss leaves the function (an image site) the join has the hit
    // edge alone and the test folds away; in the resume twin, which keeps
    // the miss, it is still the test. Keyed by the result Value, which the
    // flag dominates, so a copy of the read lowered elsewhere (a static run's
    // two copies) has its own or none; a value is found through the bitcasts
    // between `i64` and `tagged` a result is stored as. Per function.
    [[nodiscard]] Value* number_flag(Value* v) const;
    void set_number_flag(Value* v, Value* flag) { number_flags_[v] = flag; }
    void clear_number_flags() {
        number_flags_.clear();
        object_flags_.clear();
        plain_proofs_.clear();
        next_proof_ = 0;
    }
    // Per class family (TranslatorOptions::class_family_object_slots), the
    // slots of its `object` fields.
    void set_class_family_objects(const std::vector<uint64_t>* objects) { family_objects_ = objects; }
    // Whether `site`'s claim makes its slot an OBJECT one (runtime/slot_repr.h):
    // a layout cell member marked `object`, or a family field every class of
    // the site's range marks so. The runtime publishes the cell, or stamps
    // the class, only on a shape whose slot is one, so behind the guard the
    // slot holds a plain object, and a store there must bring one.
    [[nodiscard]] bool object_member(const StaticSlotSite& site) const;
    // OBJECT FLAGS, the NUMBER FLAGS' sibling: an i32 that says whether a
    // value is a plain object (an Object-tagged pointer whose header names
    // HeapKind::Plain). A read of an `object` member sets it, 1 on the edge
    // its guard held and the test itself on the miss; a receiver test of the
    // value (plain_shape_or_miss, keyed_shape, receiver_shape) asks the flag
    // instead of the tag and header, and nothing at all where it is the
    // constant. Per function, cleared with the number flags.
    [[nodiscard]] Value* object_flag(Value* v) const;
    void set_object_flag(Value* v, Value* flag) { object_flags_[v] = flag; }
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
    // Whether `v` is a plain object, as an i32, with no branch: the header is
    // read through a mapped block when `v` is no object (receiver_shape).
    Value* plain_object_test(Builder& b, Value* v);
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
    // the value bare — as one i1 before one branch. `keyed` is `obj`'s keyed
    // shape (keyed_shape), or the shape word itself where only a plain
    // receiver reaches (plain_shape_or_miss). `ptr` is the receiver's
    // payload, meaningful only where the i1 holds.
    // `family_id`, when given, is the receiver's family id (family_id below)
    // from an earlier guard, or null; a guard that reads it hands it back.
    Value* static_guard(Builder& b, Value* obj, Value* keyed, const std::vector<StaticGuardTerm>& terms,
                        Value*& ptr, Value** family_id = nullptr);
    // A keyed shape's FAMILY ID: its family stamp less the module's first
    // family id, so that a site's range test is `id - lo <= span` with both
    // constants. A fact about the shape, as the keyed shape is one about the
    // receiver, and remembered beside it (il_shape_memo.cpp).
    Value* family_id(Builder& b, Value* keyed);
    // A receiver's shape word, reached only when it is a plain object: the
    // object test and the plain-header test as two branches to `miss`, the
    // builder left in the block where both held. What a guard with no keyed
    // shape in hand (no memo entry) asks instead of keyed_shape. With
    // `exits` the two tests are image sites whose generic code is `miss`.
    Value* plain_shape_or_miss(Builder& b, Value* obj, BasicBlock* miss, bool exits = false);
    // SpecSiteEmitter::site_branch, for the sites lowered here.
    void site_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow, bool exits,
                     const std::vector<Value*>& fast_args = {});
    // SLOW ARMS (il_speculation.h): the instruction being lowered's slow
    // site, and whether its slow arms are twin exits; set by IlLowering for
    // every instruction. `slow_branch` is site_branch to a slow arm, `slow_arm`
    // starts one (its counter in a build that may be harvested): every slow
    // block of an inline fast path whose generic code runs the instruction
    // from its start, with nothing done yet that it would do again. A run's
    // slow arm runs all of its instructions, so it carries (and counts) every
    // slow site among them: the harvested build may have formed other runs.
    void set_slow_sites(std::vector<uint32_t> sites, bool exits) {
        slow_sites_ = std::move(sites);
        slow_exits_ = exits;
    }
    [[nodiscard]] bool slow_exits() const noexcept { return slow_exits_; }
    void slow_branch(Builder& b, Value* hit, BasicBlock* fast, BasicBlock* slow,
                     const std::vector<Value*>& fast_args = {}) {
        site_branch(b, hit, fast, slow, slow_exits_, fast_args);
    }
    void slow_arm(Builder& b) {
        if (!slow_count_fn_) return;
        for (const uint32_t site : slow_sites_) slow_count_fn_(b, site);
    }
    void set_slow_count_fn(std::function<void(Builder&, uint32_t)> fn) { slow_count_fn_ = std::move(fn); }
    // ARMS (il_speculation.h): the alternatives of the instruction's inline
    // path, each an index after its slow site's (below kSlowSiteArms).
    // `arm_begin` starts one, marking it run in a build that may be
    // harvested; `arm_exits` says the harvested build reached the
    // instruction's lowering there and never ran the arm, in a function with
    // a twin. `only_arm` is the one arm of [first, last] that ran, where
    // every other and the slow arm never did (else 0): its test alone is
    // then the speculated function's, an exit into the whole ladder.
    // A run of instructions has no arms.
    void set_arm_fns(std::function<bool(uint32_t)> cold, std::function<void(Builder&, uint32_t)> mark) {
        arm_cold_fn_ = std::move(cold);
        arm_mark_fn_ = std::move(mark);
    }
    [[nodiscard]] bool arm_exits(uint32_t arm) const {
        return slow_sites_.size() == 1 && arm_cold_fn_ && arm_cold_fn_(slow_sites_[0] + arm);
    }
    void arm_begin(Builder& b, uint32_t arm) {
        if (arm_mark_fn_ && slow_sites_.size() == 1) arm_mark_fn_(b, slow_sites_[0] + arm);
    }
    [[nodiscard]] uint32_t only_arm(uint32_t first, uint32_t last) const;
    // One test of a ladder: `br_if cond, hit, miss`, an exit where
    // `miss_cold` says every arm behind it, and the slow arm, never ran.
    void arm_test(Builder& b, Value* cond, BasicBlock* hit, BasicBlock* miss, bool miss_cold);
    // THE KEYED SHAPE of `obj`: its shape word when it is a plain object,
    // else the stand-in shape (not_plain_shape). One i64 that answers
    // "plain, and of shape S" with one compare against S, for every S a guard
    // compares it with: a static cell holds 0, 1 (refused) or a Shape*, a
    // method site's way word 0, a Shape* or an odd exotic encoding
    // (bronze_abi_layout.h), and a Shape* is arena memory, never the module
    // data or the block the stand-in is. And it is always a readable shape:
    // a guard reads its family stamp and double-slot map with no plainness
    // test, and the stand-in's are UNSTAMPED (which no family range admits)
    // and empty (every slot boxed).
    //
    // keyed_shape branches (the object and header tests, to a cold block
    // that passes the join the stand-in) and leaves the builder in the join;
    // keyed_shape_flat is the same answer from selects, in the current block,
    // for a point that cannot be split (a miss edge's recompute, before its
    // terminator).
    Value* keyed_shape(Builder& b, Value* obj);
    Value* keyed_shape_flat(Builder& b, Value* obj);
    Value* not_plain_shape(Builder& b);
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
    // The fill's test for a published identity cell the receiver's shape
    // grew from (il_static_slot.cpp): to `bb_publish` with the cell cleared,
    // else to `bb_done`.
    void emit_static_regrow(Builder& b, Value* obj, Value* cell, Value* cells, int32_t off, BasicBlock* bb_publish,
                            BasicBlock* bb_done, const std::string& prefix);
    // One static read: the guard, a constant-offset load on a hit, and on a
    // miss the site's ordinary sequence and then the fill.
    Value* lower_prop_get_static(Builder& b, Value* obj, uint32_t key_index, const StaticSlotSite& site,
                                 const StaticMiss& miss);
    // One static write, likewise; `need_barrier` false for a primitive value.
    void lower_prop_set_static(Builder& b, Value* obj, uint32_t key_index, Value* val, const StaticSlotSite& site,
                               bool val_is_number, bool need_barrier, const StaticMiss& miss);
    // A SHAPE-GUARDED site (il.h `shapeGuarded`): a dominating `is.shape`
    // already answered `site`'s guard, so the read is the slot's load and the
    // write, of a Number, the slot's store with no barrier.
    Value* lower_prop_get_guarded(Builder& b, Value* obj, const StaticSlotSite& site);
    void lower_prop_set_guarded(Builder& b, Value* obj, Value* val, const StaticSlotSite& site);
    // `is.shape`: `site`'s guard alone, as an i1, over the receiver's keyed
    // shape `keyed`.
    Value* lower_is_shape(Builder& b, Value* obj, Value* keyed, const StaticSlotSite& site);

    // With per-thread module data (TranslatorOptions::per_thread_module_data)
    // the table's address is the symbol plus the calling thread's delta,
    // which the owner of the function being lowered supplies; unset, the
    // symbol's own address is the table.
    void set_module_delta_fn(std::function<Value*(Builder&)> fn) { module_delta_fn_ = std::move(fn); }
    // The function being lowered hands in its table address, computed once
    // at its entry; unset, every site computes its own.
    void set_table_base_fn(std::function<Value*(Builder&)> fn) { table_base_fn_ = std::move(fn); }
    // A site's way-0 shape and slot words, from its packed row
    // (bronze_abi_layout.h, "the HOT array"), addressed off the site's own
    // base: what every way-0 guard compares and loads. hot_row_ptr is the
    // row's address, which the read stubs take beside the site's.
    int32_t hot_row_off(const IcSite& site) const;
    Value* hot_row_ptr(Builder& b, const IcSite& site);
    Value* way0_shape(Builder& b, const IcSite& site);
    Value* way0_slot_word(Builder& b, const IcSite& site);
    // The static-slot guards' two tables (the cells, and the family base
    // word) as the function addresses them: `family` names which. Unset,
    // every guard computes its own.
    void set_static_base_fn(std::function<Value*(Builder&, bool family)> fn) { static_base_fn_ = std::move(fn); }

    // Site `ic_index`, or a null site when the index names none in this
    // module's table (kNoIcIndex, or a module compiled without one).
    IcSite ic_site_ref(Builder& b, uint32_t ic_index);
    // The address of the site's way 0: what the bronze helpers take as their
    // BRONZE_ABI_MU64 operand.
    Value* site_ptr(Builder& b, const IcSite& site);
    // ic_site_ref's address, or null.
    Value* ic_site(Builder& b, uint32_t ic_index);
    // The method row of METHOD-CALL site `ic_index` (bronze_abi_layout.h,
    // "the METHOD ROWS"), or null for an index that names no method site.
    Value* method_row(Builder& b, uint32_t ic_index);

    // The runtime's id for key constant `key_index` (read through the
    // module's key map), or kNoKey as itself.
    Value* runtime_key(Builder& b, uint32_t key_index);

    // `bronze_prop_get` on key constant `key_index`, through the site
    // `ic_entry` (null: no site).
    Value* lower_prop_get(Builder& b, Value* obj, uint32_t key_index, const IcSite& site);

    // A bronze property read with the monomorphic inline hit in front of the
    // helper: the receiver's shape word against the site's way 0 and its
    // slot word below the inline-slot count (bronze_abi.h,
    // BRONZE_ABI_IC_SLOTWORD_OFFSET), one slot load on a hit and
    // `bronze_prop_get` on a miss. `key_index` is the module's key constant.
    // Requires a real site.
    Value* lower_prop_get_mono(Builder& b, Value* obj, uint32_t key_index, const IcSite& site,
                               bool expect_poly = false);

    // A read predicted to answer `undefined` for some receivers (il.h
    // `icAbsent`): way 0 of the site, taken inline when it names the
    // receiver's shape with an own data slot (the slot) or, while the
    // prototype epoch holds, with the negative entry (`undefined`); anything
    // else is the stub. An image's only: the epoch is read through the pinned
    // TLS register. Requires a real site.
    Value* lower_prop_get_absent(Builder& b, Value* obj, uint32_t key_index, const IcSite& site);

    // A read a profile saw meet 1..4 shapes (il.h `icChain`,
    // il_prop_get_chain.cpp): that many ways compared inline, the matching
    // one's own data slot, negative entry or (kChainProto) one-link
    // prototype data answered there, the stub for the rest. An image's only:
    // null (nothing emitted) for a read without a chain, a site or IC stubs,
    // and for code that runs once.
    Value* lower_prop_get_chain(Builder& b, Value* obj, const BronzeInstruction& inst_ast, const IcSite& site,
                                bool run_once, const MemoCarry* carry);

    // The memo carry the next mono or absent read takes (and clears), so
    // that the read keeps the shape memo across its join; null leaves the
    // read as it was, the memo forgotten after it.
    void set_memo_carry(const MemoCarry* carry) { memo_carry_ = carry; }

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
    // load_slot as a branch on the slot word (inline or overflow), leaving
    // the builder in the join: for a point that may be split.
    Value* load_slot_br(Builder& b, Value* ptr, Value* slot_word);

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
    // Whether the helper calls of lower_prop_get / lower_prop_set (and the
    // misses of the inline paths) go through kPropGetStub / kPropSetStub,
    // which the module then defines (emit_direct_stubs).
    void set_ic_stubs(bool on) noexcept { ic_stubs_ = on; }
    [[nodiscard]] bool ic_stubs() const noexcept { return ic_stubs_; }
    // Whether a computed access's helper call goes through kElemGetStub /
    // kElemSetStub (il_elem_stub.h), which the module then defines.
    void set_elem_stubs(bool on) noexcept { elem_stubs_ = on; }
    [[nodiscard]] const char* elem_get_helper() const noexcept;
    [[nodiscard]] const char* elem_set_helper() const noexcept;
    [[nodiscard]] const char* index_get_helper() const noexcept;
    [[nodiscard]] const char* index_set_helper() const noexcept;
    // The module's key constants, which name a property site in a profile.
    void set_key_names(const std::vector<std::string>* names) noexcept { key_names_ = names; }
    [[nodiscard]] std::string_view key_tag(uint32_t key_index) const noexcept {
        return key_names_ && key_index < key_names_->size() ? std::string_view((*key_names_)[key_index])
                                                            : std::string_view();
    }

    // `flags` are the element flags (il_pin.h): with kElemFlagArrayReceiver
    // the receiver is a proven Array (il_elem_array.cpp).
    Value* lower_elem_get(
        Builder& b,
        Value* obj,
        Value* index,
        int64_t flags = 0
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
        uint32_t strict,
        int64_t flags = 0
    );
    // The proven-Array forms and `is.array` (il_elem_array.cpp).
    static bool elem_array_receiver(int64_t flags);
    Value* lower_elem_get_array(Builder& b, Value* obj, Value* index, int64_t flags);
    void lower_elem_set_array(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict, int64_t flags);
    Value* lower_is_array(Builder& b, Value* obj);
    Value* lower_is_plain_array(Builder& b, Value* obj);
    // CreateDataPropertyOrThrow (elem.define) on an open Array, inline (il_elem_array.cpp).
    void lower_elem_define_array(Builder& b, Value* obj, Value* key, Value* val);
    // `key in obj` (il_elem_array.cpp): an Array's own element answered inline,
    // and with `elem_out` the slot it loaded (the hole on every other arm).
    Value* lower_has_index_array(Builder& b, Value* key, Value* obj, Value** elem_out = nullptr,
                                 int64_t flags = 0);
    // A proven Array's `length` (il_pin.h kPropGetArrayLength), its box.
    Value* lower_array_length(Builder& b, Value* obj);
    // `$LengthOfArrayLike(O)` with an Array's header length inline.
    Value* lower_length_of_array_like(Builder& b, Value* obj);
    // A read whose `in` loaded its element (il_in_elem_fuse.cpp): `loaded`
    // when no hole, else `full`, the read's own lowering, laid out cold.
    Value* lower_elem_get_loaded(Builder& b, Value* loaded, const std::function<Value*(Builder&)>& full);
    // The proven word-view forms (il_elem_word_view.cpp).
    static bool elem_word_view_receiver(int64_t flags);
    Value* lower_elem_get_word_view(Builder& b, Value* obj, Value* index, int64_t flags);
    void lower_elem_set_word_view(Builder& b, Value* obj, Value* index, Value* val, uint32_t strict,
                                   int64_t flags);

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
    // `is.number_array` (il_number_array.cpp): whether `obj` is an Array with
    // its NUMBERS bit set and `max_index < length`, as an i32; a clear bit on a
    // short array is claimed by a scan first.
    Value* lower_is_number_array(Builder& b, Value* obj, uint32_t max_index, bool proven_dense);
    // Clears the NUMBERS bit of the Array `obj` (a tagged i64 known to be
    // one): what an element store owes when the value it stored may not be a
    // Number in double form (il_number_array.cpp says where).
    void forget_numbers(Builder& b, Value* obj);
    // The same, only when `val` (a boxed i64) is tagged above the Number range.
    void forget_numbers_unless_number(Builder& b, Value* obj, Value* val);
    // THE ARRAY INDEX HAZARD as generated code meets it (il_number_array.cpp;
    // bronze_abi_tls.h, array_index_hazard): an i32, 1 while no prototype
    // anywhere could answer an array index, so a hole is `undefined` and a
    // store into a hole or at `length` is the array's own.
    Value* index_hazard_clear(Builder& b);
    // The tag mask a dense-array guard ANDs the `elements` word with: the
    // object tag's mask while the hazard is clear, 0 once it is set
    // (bronze_abi_tls.h, array_elems_tag_mask).
    Value* index_hazard_tag_mask(Builder& b);
    // The hazard word itself, an i64: 0 while clear, 0xFFFFFFFF once set, so
    // an index OR'd with it is below no 32-bit bound.
    Value* index_hazard_word(Builder& b);
    // An element `elem` loaded from an Array's block: a non-hole goes to `done`
    // as itself, a hole to `done` as `undefined` while the hazard is clear and
    // to `slow` (no arguments) otherwise, from a block named cold.
    // A new block, with the builder left on the one it was emitting.
    BasicBlock* append_block_keeping_position(Builder& b, const std::string& name);
    void finish_hole_read(Builder& b, Value* elem, BasicBlock* done, BasicBlock* slow, const std::string& prefix);
    // In front of an inline store into slot `slot` of the elements block
    // `block`: an element there goes to `store`, a hole to `store` while the
    // hazard is clear and to `slow` otherwise.
    void branch_on_hole_store(Builder& b, Value* block, Value* slot, BasicBlock* store, BasicBlock* slow,
                              const std::string& prefix);
    // The Array arm of an inline element store, from the header loads to
    // `store` (il_number_array.cpp): emitted where `ptr` is known an Array and
    // `idx` (an i64) a valid index. Sets the elements block and the slot the
    // store writes; every refusal branches to `slow`.
    void branch_on_array_store(Builder& b, Value* ptr, Value* idx, BasicBlock* store, BasicBlock* slow,
                               const std::string& prefix, Value*& out_block, Value*& out_slot);
    // Whether `val` is a Number in double form by construction: an f64, a box
    // of one, or a constant at or below the Number range.
    bool is_known_number(Value* val) const;
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
    // `as` i64 (the Value, a hole mapped to undefined with `check_hole`) or
    // f64 (the same eight bytes as a double, hole and all: il.h `numberBits`).
    Value* load_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index, bool check_hole,
                                        Type as = Type::i64());
    // `val` an i64 Value, or an f64 the caller has proven no NaN (its eight
    // bytes are then the Number's Value as they stand).
    void store_proven_dense_array_elem(Builder& b, Value* block, Value* head, uint32_t index, Value* val);
    bool is_known_non_ref(Value* val) const;

    // Guard for an Array read/write run over constant indices `0..max_index`:
    // sets `out_ok`, `out_block` and `out_head`. It also requires the array
    // index hazard clear (through the TLS tag mask, at no cost), since what
    // it licenses reads a hole as `undefined` and stores over one unlooked.
    // With `miss`, a value that is no object or no Array branches there (the
    // caller's slow arm: slow_branch, so `miss` must start with slow_arm) and
    // `out_ok` is the rest, the builder left where both tests held.
    void emit_dense_array_guard(Builder& b, Value* obj, uint32_t max_index, bool writable,
                                Value*& out_ok, Value*& out_block, Value*& out_head, BasicBlock* miss = nullptr);

    // A constant-index property read or write (`o[0]`, `o[15] = v`): an inline
    // Array element access in front of the computed access's helper, called
    // with the index as a Number.
    Value* lower_prop_get_index(Builder& b, Value* obj, uint32_t elem_index, bool check_hole = true);
    void lower_prop_set_index(Builder& b, Value* obj, uint32_t elem_index, Value* val, uint32_t strict,
                              bool val_is_primitive = false);
    // The same access past the Array test (il_property_lowering.cpp).
    Value* lower_index_get_helper(Builder& b, Value* obj, uint32_t elem_index);
    void lower_index_set_helper(Builder& b, Value* obj, uint32_t elem_index, Value* val, uint32_t strict);

    // `obj.length` with inline Array and typed-array arms (each header's
    // length) in front of the site's way-0 guard (`mono`) or the helper.
    Value* lower_prop_get_length(Builder& b, Value* obj, uint32_t key_index, const IcSite& site, bool mono,
                                 bool is_family);

    void lower_method_def(Builder& b, Value* obj, uint32_t key_index, Value* closure);

    // A read at a site whose receiver names a class (il_fn_recv.cpp): the
    // function arms, and `ordinary` for every other receiver.
    Value* lower_prop_get_fn_recv(Builder& b, Value* obj, uint32_t key_index, const IcSite& site,
                                  const std::function<Value*(Builder&)>& ordinary);

    // `define.own.attr`: the helper, behind the inline add its cells hold
    // (il_define_add.cpp) when `arm` and the module backs `cell`.
    static constexpr uint32_t kNoDefineAddCell = UINT32_MAX;
    void lower_define_own_attr(Builder& b, Value* obj, Value* key_id, Value* val, uint32_t mask, uint32_t cell,
                               bool arm, bool val_is_number);

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
    // own data slot. A null `plain` is a shape the caller reached only for a
    // plain receiver. A write passes `number_store`, its value is a Number,
    // and then also hits a double slot's entry (whose slot word it masks).
    void emit_mono_slot_guard(Builder& b, Value* shape, Value* plain, const IcSite& site,
                              Value*& hit, Value*& slot_word, Value* number_store = nullptr);

    // Helpers for batched element-offset runs (il_lowering_elem_run.cpp).
    void emit_elem_index_guard(Builder& b, Value* index, Value*& out_i64, Value*& out_ok,
                               bool proven_integral = false);
    Value* unbox_known_number(Builder& b, Value* val, Value*& out_ok);
    Value* box_number(Builder& b, Value* f64_val);
    void object_payload(Builder& b, Value* obj, Value*& out_ptr, Value*& out_is_obj);
    Value* load_typed_array_data(Builder& b, Value* ptr);

private:
    // The shared test of the two mono paths: `hit`, the receiver's payload
    // and the site's slot word.
    void mono_hit(Builder& b, Value* obj, const IcSite& site, Value*& hit, Value*& ptr, Value*& slot_word);
    // The read's poly test: the receiver's shape against the site's ways
    // 1..N-1, with the matching way's slot word. `plain` is null when `shape`
    // is the keyed shape, which says plainness itself.
    void other_ways_hit(Builder& b, Value* plain, Value* shape, const IcSite& site, Value*& hit,
                        Value*& slot_word);
    // The edges into a read's join and the call-free ones among them: the
    // memo carry's values appended to each, and the join's new parameters
    // handed back to it.
    static void carry_memo(Builder& b, const MemoCarry& carry, BasicBlock* merge,
                           const std::vector<std::pair<BasicBlock*, bool>>& edges);
    const MemoCarry* memo_carry_ = nullptr;
    SpecSiteEmitter* spec_ = nullptr;
    bool inline_sets_ = true;
    bool feedback_driven_ = false;
    bool in_process_ = false;
    bool ic_stubs_ = false;
    bool elem_stubs_ = false;
    const std::vector<std::string>* key_names_ = nullptr;
    bool enable_inlined_fastpaths_ = true;
    std::string key_map_sym_ = "__bronze_key_map";
    std::string ic_table_sym_ = "__bronze_ic_table";
    uint32_t ic_site_count_ = 0;
    std::unordered_map<uint32_t, uint32_t> method_rank_;
    const std::unordered_map<uint32_t, LayoutCell>* layout_cells_ = nullptr;
    std::unordered_map<Value*, Value*> number_flags_;
    std::unordered_map<Value*, Value*> object_flags_;
    std::unordered_map<Value*, std::vector<uint32_t>> plain_proofs_;
    uint32_t next_proof_ = 0;
    const std::vector<uint64_t>* family_objects_ = nullptr;
    std::string static_cells_sym_;
    uint32_t static_cell_count_ = 0;
    std::string family_base_sym_;
    uint32_t family_count_ = 0;
    // A writable table's address on this thread (module_delta_fn_).
    Value* data_addr(Builder& b, const std::string& sym);
    Value* unshared_data_addr(Builder& b, const std::string& sym);
    std::function<Value*(Builder&)> module_delta_fn_;
    std::function<Value*(Builder&, bool)> static_base_fn_;
    std::function<Value*(Builder&)> table_base_fn_;
    std::function<void(Builder&, uint32_t)> slow_count_fn_;
    std::function<bool(uint32_t)> arm_cold_fn_;
    std::function<void(Builder&, uint32_t)> arm_mark_fn_;
    std::vector<uint32_t> slow_sites_;
    bool slow_exits_ = false;
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
