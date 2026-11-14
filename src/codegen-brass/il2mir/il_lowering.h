#pragma once

#include "abi/bronze_abi.h"
#include "il_ast.h"
#include "il_translator.h"
#include "il_property.h"
#include "il_alloc_lowering.h"
#include "il_speculation.h"
#include <brass/mir/builder.hpp>
#include <brass/core/diagnostics.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace il2mir {

// The type a Bronze value of type `t` is computed in: a dynamic value's
// bits are an i64.
Type lower_type(BronzeType t);
// The type a Bronze value of type `t` is held in across instructions (SSA
// results, block and function parameters, returns): a dynamic value is
// tagged, the MIR type every tier's stack maps report to the collector.
Type lower_abi_type(BronzeType t);
// An instruction that neither calls, allocates, throws nor touches the heap:
// a constant, a scalar operation, a box of a scalar, a proven unbox
// (il_lowering_hoist.cpp).
bool is_pure_scalar_inst(const BronzeInstruction& inst);
class IlLowering;

// Dynamic call helper dispatch (`bronze_call_dynamic_0..16` or staged `_n`)
// and typed-edge argument boxing shared by call and builtin lowerings.
Value* build_call_dynamic(Builder& b, const std::vector<Value*>& dyn_args, IlLowering* lowering);
Value* box_call_arg(IlLowering* lowering, Builder& b, Value* v, Type as);

class IlLowering {
public:
    IlLowering(const TranslatorOptions& options, DiagnosticReporter* diag = nullptr);

    std::unique_ptr<Module> lower_module(const BronzeModuleAST& ast);
    // Lazy bodies (TranslatorOptions::lazy_bodies): builds the body of `fn`,
    // a function lower_module left lazy, from the AST it was given (which
    // must outlive this object, as must this object the module). The
    // module's body provider; run one at a time.
    bool lower_lazy_body(Function& fn);
    Value* ensure_type(Value* val, Type target_type, Builder& b);
    std::string resolve_callee(const std::string& callee_name) const;
    std::string resolve_create_func_callee(const std::string& callee_name);
    Value* get_key_id(Builder& b, uint32_t key_idx);
    uint32_t find_key_constant(const std::string& name) const;

    Value* get_val_by_id(uint32_t id, Builder& b, const std::unordered_map<uint32_t, Value*>& val_map);
    void set_inst_result(uint32_t result_id, Value* res_val, Builder& b, std::unordered_map<uint32_t, Value*>& val_map);
    // The bits of a tagged value, read at this point.
    Value* unbox_tagged(Value* tagged, Builder& b);
    // Stores `args` into the function's argv block (an alloca.tagged that
    // lower_function sizes for its widest argv call) and returns its
    // address, or 0 for no arguments.
    Value* stage_argv(Builder& b, const std::vector<Value*>& args);
    PropertyLoweringHelper& prop_lowering() { return prop_lowering_; }
    AllocLoweringHelper& alloc_lowering() { return alloc_lowering_; }
    SpecSiteEmitter& spec() { return spec_; }
    // Whether a claimed property site's miss leaves the function for its
    // resume twin (il_speculation.h, "IMAGE SITES"): an image built on a
    // profile that did not see the claim miss.
    bool claim_exits(const BronzeInstruction& inst) const {
        return options_.twin_exits && inst.static_slot != BronzeInstruction::kNoStaticSlot &&
               inst.ic_index != BronzeInstruction::kNoIcIndex && options_.missed_claim_ics.count(inst.ic_index) == 0;
    }
    // Whether `inst`'s slow arms leave the function for its resume twin
    // (il_speculation.h, "SLOW ARMS"): an image built on a harvest whose
    // build counted this instruction's generic code and never saw it run, in
    // a function that has a twin.
    bool slow_exits(const BronzeInstruction& inst) const {
        return options_.twin_exits && spec_.image_sites_here() && inst.slow_site != BronzeInstruction::kNoSlowSite &&
               options_.slow_cold.count(inst.slow_site) != 0;
    }
    // Makes `inst`'s slow site the one the slow arms lowered next belong to.
    void set_slow_site_of(const BronzeInstruction& inst) {
        std::vector<uint32_t> sites;
        if (inst.slow_site != BronzeInstruction::kNoSlowSite) sites.push_back(inst.slow_site);
        prop_lowering_.set_slow_sites(std::move(sites), slow_exits(inst));
    }
    // A run of instructions [from, to) of `insts` (taken again after it
    // lowers others): its slow arm is every slow site of the run's, and exits
    // only when every one is cold (armed, never run). The harvested
    // build's runs need not be these, and each of its runs counted all of its
    // sites, so a slow path it took shows on every site that took it.
    // `may_exit` false keeps the arm a branch (it still counts): a constant-
    // index run's exit holds every element it loaded in its state, which cost
    // three.js's Matrix4.multiplyMatrices 28% more instructions.
    void set_run_slow_site(const std::vector<BronzeInstruction>& insts, size_t from, size_t to,
                           bool may_exit = true) {
        std::vector<uint32_t> sites;
        bool exits = may_exit;
        for (size_t i = from; i < to; ++i) {
            if (insts[i].slow_site == BronzeInstruction::kNoSlowSite) continue;
            sites.push_back(insts[i].slow_site);
            exits = exits && slow_exits(insts[i]);
        }
        exits = exits && !sites.empty();
        prop_lowering_.set_slow_sites(std::move(sites), exits);
    }
    const TranslatorOptions& options() const { return options_; }
    const BronzeModuleAST* current_ast() const { return current_ast_; }
    const BronzeFunction* current_fn_ast() const {
        return (current_ast_ && current_fn_idx_ < current_ast_->functions.size())
                   ? &current_ast_->functions[current_fn_idx_]
                   : nullptr;
    }
    BasicBlock* entry_hoist_block() const { return hoist_block_; }
    // Whether the function being lowered is a class constructor's body
    // (BRONZE_ABI_FN_FLAG_CLASS_CTOR), whose writes to `this` add properties.
    bool in_class_constructor() const {
        const BronzeFunction* f = current_fn_ast();
        if (!f) return false;
        const auto it = options_.function_meta.find(f->name);
        return it != options_.function_meta.end() && (it->second.fn_flags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) != 0;
    }
    // The key index IL value `id` was read with, if a `prop.get` of a named
    // key made it in the current function (a builtin read into a local, then
    // called through it).
    uint32_t prop_get_key_of(uint32_t id) const {
        auto it = prop_get_keys_.find(id);
        return it == prop_get_keys_.end() ? UINT32_MAX : it->second;
    }
    // A typed array's cursor (il_property_lowering.cpp get_preloaded_ta_cursor):
    // whether the array may have one, decided once per function; what is read
    // once, at the array's definition: its buffer (tagged, so the collector
    // keeps it current), the mask and offset that make the window's first
    // byte from the buffer's address, and the length; and the data pointer
    // as last derived from those, null once forgotten (il_ta_cursor.cpp).
    struct TaCursor {
        bool eligible = false;
        bool loaded = false;
        Value* buf = nullptr;
        Value* keep = nullptr;
        Value* off = nullptr;
        Value* len = nullptr;
        Value* data = nullptr;
    };
    std::unordered_map<uint32_t, TaCursor>& ta_cursors() { return ta_cursors_; }
    DiagnosticReporter* diag() const { return diag_; }
    void set_diag(DiagnosticReporter* diag) { diag_ = diag; }
    void set_has_error(bool e) { has_error_ = e; }
    std::string module_sym(const std::string& base) const {
        if (options_.entry_symbol.empty() || options_.entry_symbol == "main" || options_.entry_symbol == "bronze_main") {
            return base;
        }
        return base + "_" + options_.entry_symbol;
    }
    // The calling thread's address of one of the module's WRITABLE tables
    // (`base` unsuffixed): the symbol itself, or — with per-thread module
    // data — the symbol plus this thread's delta
    // (TranslatorOptions::per_thread_module_data).
    Value* module_data_addr(Builder& b, const std::string& base);
    // This thread's delta for the module being lowered: the value the
    // function computed once at entry, or a fresh load where it has none.
    Value* module_delta(Builder& b);
    // In a coroutine body: its frame (the leading parameter), and the brass
    // state id of its next suspension, which carries the BRONZE_ABI_SUSPEND_*
    // kind in its low two bits (bronze_abi.h).
    Value* coro_frame() const { return coro_frame_val_; }
    uint32_t next_coro_state_id(uint32_t kind) {
        return (++coro_suspend_count_ << 2) | (kind & BRONZE_ABI_SUSPEND_KIND_MASK);
    }
    // Whether the IL block being lowered runs at most once per call of a
    // function that runs once: the program's top level outside any loop.
    // Its operations call their helpers, with no inline fast path — a path
    // run once is only size, and every inline cache misses its first run.
    bool run_once() const { return run_once_; }
    // Whether the IL block being lowered is on a cycle of its function's CFG
    // (a loop body, or a block a handler re-enters): code that runs many
    // times per call, where a site's inline path pays for its size.
    bool in_loop() const { return in_loop_; }
    // Whether the IL block being lowered has a handler: its calls become
    // invokes (il_lowering_eh.cpp), which name their callee, so a call through
    // a code pointer cannot be one and its site takes the helper.
    bool in_protected_block() const { return protected_block_; }
    // Whether IL value `id` is an object this function created
    // (CreateObject): a write to it adds a property.
    bool is_fresh_object(uint32_t id) const { return fresh_objects_.count(id) != 0; }
    // Whether IL value `id` is an Array on every path: a `create.array`'s
    // result, or a block parameter each of whose incoming arguments is one
    // (`events = []` carried round a loop). An Array's heap kind never
    // changes, so a site that tests its receiver for being one need not.
    bool is_array_value(uint32_t id) const { return array_value_ids_.count(id) != 0; }
    // Whether every use of `id` in the function is `IsNumber` or a raw `Unbox`.
    bool is_hole_insensitive(uint32_t id) const { return hole_insensitive_ids_.count(id) != 0; }
    // Whether `id` is statically guaranteed to be a primitive value (never a
    // heap reference), so storing it requires no GC write barrier.
    bool is_primitive_value(uint32_t id) const { return primitive_value_ids_.count(id) != 0; }
    // Whether `id` is a `box.f64`: a Number's bits, the word a double slot
    // holds (slot_repr.h), so a store of it needs no representation test.
    bool is_number_value(uint32_t id) const { return number_value_ids_.count(id) != 0; }
    // Whether `id` is a string literal, `box.str k` with no operand: a String
    // whatever path reaches it, so `===` against it needs no Number test.
    bool is_string_literal(uint32_t id) const { return string_literal_ids_.count(id) != 0; }
    bool is_typeof_value(uint32_t id) const { return typeof_value_ids_.count(id) != 0; }
    bool is_elided_typeof(uint32_t id) const { return elided_typeof_ids_.count(id) != 0; }
    bool is_elided_typeof_lit(uint32_t id) const { return elided_typeof_lit_ids_.count(id) != 0; }
    const std::string& string_literal_text(uint32_t id) const {
        static const std::string kEmpty;
        auto it = string_literal_texts_.find(id);
        return it != string_literal_texts_.end() ? it->second : kEmpty;
    }
    // The module's class layouts handed to the runtime at init
    // (il_static_slot.cpp); `map_addr` is the filled key map.
    void register_class_families(Builder& b, Value* map_addr);
    void register_slot_repr(Builder& b, Value* map_addr);
    // Whether `arr_id` is proven at the current lowering point to be a dense
    // Array with `index < length` and a valid elements block.
    bool is_proven_dense_array(uint32_t arr_id, uint32_t index) const;
    void get_dense_array_cursor(uint32_t arr_id, Value* obj_val, Builder& b, Value*& out_block, Value*& out_head);
    void invalidate_dense_array_proofs();
    // An `is.number_array` answer lowered earlier in this block, standing
    // while the proofs do (nothing since could have written an element): the
    // i32 claim, when it covers `index`, or null. A read run behind it
    // tests the claim instead of a dense guard (il_lowering_index_run.cpp).
    Value* number_array_claim(uint32_t arr_id, uint32_t index) const;
    void note_number_array_claim(uint32_t arr_id, uint32_t max_index, Value* claim);
    // Environment record access, inline (il_env_lowering.cpp): `depth`
    // parent hops from `env`, then slot `index`. The TDZ read calls
    // bronze_env_get_tdz only for the uninitialized marker, which throws.
    Value* lower_env_get(Builder& b, Value* env, uint32_t depth, uint32_t index);
    Value* lower_env_get_tdz(Builder& b, Value* env, uint32_t depth, uint32_t index, uint32_t key_idx);
    void lower_env_set(Builder& b, Value* env, uint32_t depth, uint32_t index, Value* val, bool val_is_primitive);

    // THE SHAPE MEMO (il_shape_memo.cpp): per receiver (an IL value id), the
    // keyed shape last computed for it (PropertyLoweringHelper::keyed_shape),
    // kept while nothing lowered since can have moved that object's shape.
    // A receiver's keyed shape, the memo's if it holds one, else computed
    // here from `obj` (its bits) and remembered.
    Value* memo_keyed(Builder& b, uint32_t recv_id, Value* obj);
    // The memo's keyed shape for a receiver, or null; and a receiver's
    // keyed shape remembered (a shape word read where the receiver is known
    // plain is its keyed shape there).
    // Whether `inst` is a direct site standing as the miss of the guard just
    // tested (find_guard_misses).
    bool guard_missed(const BronzeInstruction& inst) const { return guard_missed_.count(&inst) != 0; }
    // The `method.ic.is` whose miss the call `site` is, when that guard has a
    // cell pair (il.h `staticCellIndex`) for the site to fill; else null.
    const BronzeInstruction* method_cell_guard(const BronzeInstruction& site) const {
        const auto it = method_cell_guards_.find(&site);
        return it == method_cell_guards_.end() ? nullptr : it->second;
    }
    // Whether `guard` is such a guard: its cell is filled, so it tests it.
    bool method_cell_filled(const BronzeInstruction& guard) const { return method_cells_filled_.count(&guard) != 0; }
    // The key index of the call that fills such a guard's cell pair, which a
    // publish of the pair names.
    uint32_t method_cell_key(const BronzeInstruction& guard) const { return method_cell_keys_.at(guard.static_cell); }
    Value* memo_lookup(uint32_t recv_id) const;
    // The memo's key for a receiver's FAMILY ID (PropertyLoweringHelper::
    // family_id), held beside its keyed shape and carried the same way: an
    // IL value id never has this bit.
    static constexpr uint32_t kMemoFamilyBit = 0x80000000u;
    static uint32_t memo_family_key(uint32_t recv_id) { return recv_id | kMemoFamilyBit; }
    void memo_add(uint32_t recv_id, Value* keyed);
    // Says the instruction being lowered kept the memo true (a static site
    // that carried it across its join, a pure guard).
    void memo_keep() { shape_memo_kept_ = true; }
    // Fills a static site's memo hooks (StaticMiss): the receiver's keyed
    // shape, and the memo carried across the site's join.
    void memo_attach(Builder& b, StaticMiss& miss, uint32_t recv_id, Value* obj,
                     const std::unordered_map<uint32_t, Value*>& val_map);
    // The memo as it stands, its receivers' keyed shapes recomputed at the
    // builder's position, and a rebinding to new values (same order): what
    // a two-armed lowering uses to carry it across its own join.
    // The carry an inline-cache read of `recv_id` keeps the memo across its
    // join with (MemoCarry); none where the memo is off.
    std::optional<MemoCarry> memo_carry(uint32_t recv_id, Value* obj,
                                        const std::unordered_map<uint32_t, Value*>& val_map);
    std::vector<std::pair<uint32_t, Value*>> memo_snapshot() const { return shape_memo_; }
    std::vector<Value*> memo_recompute(Builder& b, const std::unordered_map<uint32_t, Value*>& val_map);
    void memo_restore(std::vector<std::pair<uint32_t, Value*>> m) { shape_memo_ = std::move(m); }
    void memo_kill() { shape_memo_.clear(); }

    // `k in O` FUSED WITH `O[k]` (il_in_elem_fuse.cpp): an element read the
    // `in` before it already loaded, on the true edge with nothing between
    // that can write or collect. `in_loads_element(in_id)` says whether a
    // paired read wants the `in`'s element; the `in` hands it over with
    // `note_in_element` (the element's bits, or the hole when its inline arm
    // did not answer); `in_element_for(get_id)` is that value for a paired
    // read, else null.
    bool in_loads_element(uint32_t in_id) const;
    void note_in_element(uint32_t in_id, Value* elem) { in_elements_[in_id] = elem; }
    Value* in_element_for(uint32_t get_id) const;
    // The paired read's element flags (il_pin.h): an Array receiver proven
    // for the read is one for the `in` too, whose edge it is entered by and
    // whose operand it reads (a heap kind never changes).
    int64_t in_element_flags(uint32_t in_id) const;

    // 0/1 values (il_lowering_bool.cpp): a result the dynamic compare and
    // ToBoolean lowerings merge from comparisons, noted where it is made, so
    // a branch on its negation can branch on it with its targets swapped.
    void note_boolean(Value* v) { if (v) boolean_values_.insert(v); }
    bool is_zero_one(const Value* v) const;
    // c when `cond` is `eq c, 0` of a 0/1 value c, else null.
    Value* negated_boolean(Value* cond) const;

    // Speculated Math builtin calls (il_builtin_call.cpp): resets per-function
    // Box.f64 source tracking and callee-identity guard memoization, extracts
    // an already-unboxed f64 argument without boxing it for the fast path, and
    // lowers MethodCall / CallDynamic sites targeting Math members.
    void begin_builtin_lowering(const BronzeFunction& fn_ast);
    Value* peek_unboxed_f64(uint32_t id, const std::unordered_map<uint32_t, Value*>& val_map) const;
    Value* try_lower_builtin_method_call(const BronzeInstruction& inst, Builder& b, Value* recv,
                                         const std::vector<Type>& arg_kinds,
                                         const std::unordered_map<uint32_t, Value*>& val_map, Value* site);
    Value* try_lower_builtin_dynamic_call(const BronzeInstruction& inst, Builder& b,
                                          const std::unordered_map<uint32_t, Value*>& val_map);

private:
    // The memo at a block's start (from its one predecessor's end), after an
    // instruction (kept, or forgotten unless the instruction can move no
    // shape), and at a block's end.
    void memo_begin_block(Builder& b, const BronzeBlock& blk, BasicBlock* bb);
    void memo_after_instruction(const BronzeInstruction& inst);
    // `first_new` is where the block's own MIR blocks start in the function's
    // list (`bb` itself comes before): the span a join patches edges in.
    void memo_end_block(const BronzeBlock& blk, BasicBlock* bb, Function* fn, size_t first_new);
    // The memo at a join: what every predecessor's end holds, one value or
    // a join parameter fed along each predecessor's edges.
    void memo_join(Builder& b, const std::vector<uint32_t>& preds, BasicBlock* bb, uint32_t bb_id);
    // Per-function values computed once at the function's entry and shared
    // by every use (il_lowering_hoist.cpp): the inline-cache table's address.
    enum class Hoist : uint8_t { IcTable, Count };
    Value* hoisted(Builder& b, Hoist kind);
    // Starts a function's hoisting: values go into `block`, after whatever it
    // holds now.
    void begin_hoisting(BasicBlock* block);
    // Where the MIR stood before an instruction's lowering, and the
    // forgetting of every typed-array cursor at a block's start or after a
    // lowering that emitted a point where a collection can run
    // (il_ta_cursor.cpp).
    struct MirMark {
        BasicBlock* block = nullptr;
        Instruction* tail = nullptr;
        size_t block_count = 0;
    };
    MirMark mir_mark(Builder& b, const Function& fn) const;
    void forget_ta_cursors();
    void settle_ta_cursors(const MirMark& mark, const Function& fn);
    // The function's run-once blocks (run_once), fresh objects, and array proofs.
    void analyze_function(const BronzeFunction& fn_ast);
    void begin_block_proofs(uint32_t block_id);
    void after_instruction_proofs(const BronzeInstruction& inst_ast);
    bool try_lower_array_index_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                   std::unordered_map<uint32_t, Value*>& val_map,
                                   const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                   uint32_t* cont_counter);
    bool try_lower_elem_offset_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                   std::unordered_map<uint32_t, Value*>& val_map,
                                   const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                   uint32_t* cont_counter);
    // A template's pieces, every one a String, joined by one call
    // (il_string_join.cpp).
    bool try_lower_string_join(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b,
                               std::unordered_map<uint32_t, Value*>& val_map);
    // A constructor's stretch of adds behind one guard and one shape store
    // (il_transition_run.cpp).
    bool try_lower_transition_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                  std::unordered_map<uint32_t, Value*>& val_map,
                                  const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                  uint32_t* cont_counter);
    // A stretch of static-slot sites on one receiver behind one guard
    // (il_lowering_static_run.cpp).
    bool try_lower_static_slot_run(const BronzeBlock& blk_ast, size_t& inst_idx, Builder& b, Function* fn,
                                   std::unordered_map<uint32_t, Value*>& val_map,
                                   const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                                   uint32_t* cont_counter);
    struct DenseArrayProof {
        uint32_t arr_id = UINT32_MAX;
        uint32_t max_index = 0;
    };
    struct DenseArrayCursor {
        BasicBlock* bb = nullptr;
        Value* block = nullptr;
        Value* head = nullptr;
    };
    BasicBlock* hoist_block_ = nullptr;
    Instruction* hoist_after_ = nullptr;
    bool hoist_at_head_ = false;
    Value* hoisted_[static_cast<size_t>(Hoist::Count)] = {};
    bool run_once_ = false;
    bool protected_block_ = false;
    std::unordered_set<uint32_t> run_once_blocks_;
    std::unordered_set<uint32_t> loop_blocks_;
    bool in_loop_ = false;
    std::unordered_set<uint32_t> fresh_objects_;
    std::unordered_set<uint32_t> array_value_ids_;
    std::unordered_set<uint32_t> hole_insensitive_ids_;
    std::unordered_set<uint32_t> unrooted_hole_insensitive_ids_;
    std::unordered_set<uint32_t> primitive_value_ids_;
    std::unordered_set<uint32_t> number_value_ids_;
    std::unordered_set<uint32_t> string_literal_ids_;
    std::unordered_set<uint32_t> to_string_ids_;
    std::unordered_set<uint32_t> typeof_value_ids_;
    std::unordered_set<uint32_t> elided_typeof_ids_;
    std::unordered_set<uint32_t> elided_typeof_lit_ids_;
    std::unordered_map<uint32_t, std::string> string_literal_texts_;
    std::unordered_map<uint32_t, std::vector<DenseArrayProof>> block_dense_proofs_;
    std::unordered_map<uint32_t, uint32_t> active_dense_arrays_;
    std::unordered_map<uint32_t, std::pair<uint32_t, Value*>> number_array_claims_;
    std::unordered_map<uint32_t, DenseArrayCursor> dense_array_cursors_;
    std::unordered_map<uint32_t, TaCursor> ta_cursors_;
    std::unordered_map<uint32_t, uint32_t> single_block_propset_use_;
    std::unordered_map<uint32_t, uint32_t> value_use_counts_;
    std::unordered_map<uint32_t, uint32_t> prop_get_keys_;
    Value* coro_frame_val_ = nullptr;
    uint32_t coro_suspend_count_ = 0;
    // AST function `i`'s body, and its `__wrapper_` (a no-op for a function
    // that has none).
    // `target`, when given, is the Function to build into: the entry is
    // renamed after the module is lowered (BrassBackend), so a body built
    // again cannot find it by its lowering name.
    bool lower_body(size_t i, Module& mod, Function* target = nullptr);
    bool lower_wrapper(size_t i, Module& mod);
    // Whether AST function `i` is built up front even with lazy bodies.
    bool must_lower_eagerly(size_t i) const;
    // The bodies the module's provider builds (the lazy functions, and an
    // entry built up front in its own arena): AST index, whether it is the
    // wrapper, and what its first build took. brass may release a body once
    // its code exists and ask for it again (Module::release_bodies), and
    // the body built again must be the first one: the same speculation
    // sites (whose ids are its guards' resume ids, and whose counters its
    // code increments), so a deopt from code compiled off either build
    // finds its guard. BRONZE_MIR_REBUILD_CHECK=1 also keeps each first
    // build's text and fails a build again that differs from it.
    struct LazyBody {
        size_t index = 0;
        bool wrapper = false;
        bool built = false;
        uint32_t first_site = 0;
        uint32_t sites = 0;
        std::string first_text;
    };
    std::unordered_map<const Function*, LazyBody> lazy_bodies_;
    // Builds `fn`'s body (entry `lazy` of lazy_bodies_), first or again.
    bool build_provided_body(Function& fn, LazyBody& lazy);
    // Whether the entry can live in its own arena and be released: one
    // starting or suspending no coroutine (brass lowers those once, over the
    // module as it starts).
    bool entry_rebuildable(size_t i) const;
    // Per AST function: its MIR name, empty for a duplicate not defined.
    std::vector<std::string> resolved_names_;
    // What each wrapper's arity and closure-ness come from (CreateFunc
    // sites anywhere in the module).
    std::unordered_map<std::string, uint32_t> callee_param_counts_;
    std::unordered_set<std::string> closure_functions_;
    bool lower_function(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name,
                        Function* target = nullptr);
    bool emit_wrapper(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name, uint32_t declared_param_count, bool is_closure = false);
    bool lower_instruction(const BronzeInstruction& inst_ast, Builder& b, Function* fn,
                           std::unordered_map<uint32_t, Value*>& val_map,
                           const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                           uint32_t handler_id = UINT32_MAX,
                           uint32_t block_id = 0,
                           uint32_t* cont_counter = nullptr);

    // Exception edges (il_lowering_eh.cpp). A handler block is entered with
    // the thrown value as its one parameter, which its `exc.take` reads.
    // Every call a protected block makes becomes an `invoke` whose unwind
    // edge goes to a pad (`landing_pad` then a branch to the handler); a
    // call anywhere else is a plain call, and a throw it raises leaves the
    // function through the unwinder, so the path that does not throw
    // carries nothing.
    void add_handler_params(const BronzeFunction& fn_ast, Builder& b,
                            const std::unordered_map<uint32_t, BasicBlock*>& block_map);
    // The MIR blocks lowered from IL block `handler_id`'s protected region
    // since `first_new_block` (the index into fn->blocks() before the IL
    // block was lowered), with the IL block's own MIR block.
    void note_protected_blocks(Function* fn, BasicBlock* il_bb, size_t first_new_block, uint32_t handler_id);
    // Rewrites the noted blocks' calls into invokes; run once per function
    // after every block is lowered.
    void route_exception_edges(Builder& b, const std::unordered_map<uint32_t, BasicBlock*>& block_map);
    // The handler block's exception parameter, or null for a block that is
    // no handler.
    Value* handler_exception(uint32_t block_id) const;
    std::unordered_map<uint32_t, Value*> handler_exc_;
    std::vector<std::pair<BasicBlock*, uint32_t>> protected_blocks_;

    TranslatorOptions options_;
    DiagnosticReporter* diag_ = nullptr;
    PropertyLoweringHelper prop_lowering_;
    AllocLoweringHelper alloc_lowering_;
    SpecSiteEmitter spec_;
    uint32_t current_file_id_ = 0;
    // The debug-context file id of each BronzeModuleAST::source_files entry.
    std::vector<uint32_t> source_file_ids_;
    // Per function (BronzeModuleAST::functions), the brass inlined-scope id
    // of each of its inline frames, all recorded by lower_module
    // (record_inline_scopes) so that building a body never writes the
    // module's debug context (il_lowering_loc.cpp).
    std::vector<std::vector<uint32_t>> inline_scope_ids_by_fn_;
    bool record_inline_scopes(const BronzeModuleAST& ast, Module& mod);
    uint32_t inline_scope_id(uint32_t frame) const;
    // Sets `b`'s location to the instruction's: its own line in its own file,
    // inside the brass inlined scope of its inline frame.
    void apply_inst_loc(Builder& b, const BronzeInstruction& inst);
    bool has_error_ = false;
    const BronzeModuleAST* current_ast_ = nullptr;
    size_t current_fn_idx_ = 0;
    std::unordered_map<size_t, std::unordered_map<std::string, std::string>> caller_to_callee_map_;
    std::unordered_map<size_t, std::unordered_map<std::string, std::vector<std::string>>> caller_create_func_targets_;
    std::unordered_map<std::string, size_t> create_func_counter_;
    std::unordered_set<uint32_t> module_env_regs_;
    // The IL ids of the current function's dynamic values (held tagged).
    std::unordered_set<uint32_t> current_fn_dynamic_ids_;
    // The IL ids of the current function's element reads held as doubles
    // (il.h `numberBits`), each with whether its boxed form maps a hole to
    // undefined. val_map holds the f64; get_val_by_id hands every other use
    // the bits, made where that use is, so no GC reference to them lives on.
    std::unordered_map<uint32_t, bool> number_bits_ids_;
    Value* argv_block_ = nullptr;
    uint32_t argv_block_words_ = 0;
    // The function's per-thread module-data delta, computed at its entry;
    // null before the entry computed it.
    Value* current_module_delta_ = nullptr;
    Value* load_module_delta(Builder& b);
    // The stack-limit check the function was given at entry (pinned-register
    // mode), kept so a body that turned out to call nothing can drop it.
    BasicBlock* stack_check_entry_bb_ = nullptr;
    BasicBlock* stack_check_overflow_bb_ = nullptr;
    BasicBlock* stack_check_body_bb_ = nullptr;
    struct ExternalSig {
        Type return_type = Type::void_type();
        std::vector<Type> param_types;
    };
    std::unordered_map<std::string, ExternalSig> external_signatures_;
    std::vector<std::pair<uint32_t, Value*>> shape_memo_;
    bool shape_memo_kept_ = false;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, Value*>>> shape_memo_at_end_;
    // Per IL block: how many edges enter it, and the blocks they come from.
    std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> memo_preds_;
    // Per lowered IL block: the MIR blocks its lowering made (its own first).
    std::unordered_map<uint32_t, std::vector<BasicBlock*>> memo_spans_;
    std::unordered_set<uint32_t> memo_handler_blocks_;
    bool shape_memo_off_ = false;
    // Per lowered IL block whose end memo holds only on one of its edges (a
    // shape test that leaves, below): the IL block the other edge goes to,
    // and the memo that edge carries.
    std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<std::pair<uint32_t, Value*>>>> memo_leave_end_;
    // The memo block `pred` ends with on its edge into `succ`, or null.
    const std::vector<std::pair<uint32_t, Value*>>* memo_end_for(uint32_t pred, uint32_t succ) const;
    // A SHAPE TEST THAT LEAVES (il_speculation.h, "IMAGE SITES"). In a
    // function that takes guards, a test of a receiver's shape read only by
    // the branch right behind it is lowered at that branch: the receiver's
    // object test, its plain-header test and the compare are image sites,
    // each leaving where the branch's false edge goes, with no keyed shape
    // joined from a not-plain arm. The tests: an `is.shape` whose branch
    // leaves its fast copy, and an `accessor.ic.is` whose branch does, or
    // whose miss is its own site (find_guard_misses) and the profile saw
    // that site's cache hold one shape (TranslatorOptions::mixed_ics).
    // `next_inst_` is the instruction behind the one the block loop is
    // lowering.
    const BronzeInstruction* next_inst_ = nullptr;
    const BronzeInstruction* pending_shape_test_ = nullptr;
    Value* pending_shape_obj_ = nullptr;
    // Per guard with a miss site of its own: that site's inline-cache index.
    std::unordered_map<const BronzeInstruction*, uint32_t> guard_miss_ic_;

public:
    // Whether `test` (being lowered, receiver bits `obj`) is left to the
    // branch behind it; the caller then gives it any i32.
    bool defer_shape_test(const BronzeInstruction& test, Value* obj);

private:
    void memo_begin_function(const BronzeFunction& fn_ast);
    // A direct site that is the miss of an inlined body's guard: the first
    // instruction of the block only that guard's false edge enters, on the
    // guard's receiver and its cell or cache. Its own guard would ask what
    // was just answered no, so it lowers as its miss alone; and the guard's
    // cell words, then read nowhere else, fold into the guard's compares.
    void find_guard_misses(const BronzeFunction& fn_ast);
    std::unordered_set<const BronzeInstruction*> guard_missed_;
    std::unordered_map<const BronzeInstruction*, const BronzeInstruction*> method_cell_guards_;
    std::unordered_set<const BronzeInstruction*> method_cells_filled_;
    std::unordered_map<uint32_t, uint32_t> method_cell_keys_;
    std::unordered_set<const Value*> boolean_values_;
    // The current function's fused `in` / element read pairs: read id -> in
    // id, and each lowered `in`'s element (il_in_elem_fuse.cpp).
    std::unordered_map<uint32_t, uint32_t> in_element_reads_;
    std::unordered_map<uint32_t, Value*> in_elements_;
    std::unordered_map<uint32_t, int64_t> in_element_flags_;
    void pair_in_element_reads(const BronzeFunction& fn_ast);
    struct BuiltinFnGuard {
        uint32_t callee_id = UINT32_MAX;
        uint32_t key_index = UINT32_MAX;
        const Function* fn = nullptr;
        BasicBlock* def_bb = nullptr;
        Value* fn_ok = nullptr;
    };
    Value* lookup_builtin_fn_guard(uint32_t callee_id, uint32_t key_index, BasicBlock* cur) const;
    void note_builtin_fn_guard(uint32_t callee_id, uint32_t key_index, BasicBlock* def_bb, Value* fn_ok);
    std::unordered_map<uint32_t, uint32_t> box_f64_src_;
    std::vector<BuiltinFnGuard> builtin_fn_guards_;
};

} // namespace il2mir
