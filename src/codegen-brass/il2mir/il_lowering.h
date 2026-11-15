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
class IlLowering;

// Dynamic call helper dispatch (`bronze_call_dynamic_0..16` or staged `_n`)
// (il_lowering_calls.cpp).
Value* build_call_dynamic(Builder& b, const std::vector<Value*>& dyn_args, IlLowering* lowering);

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
    // Makes `inst`'s slow site the one the slow arms lowered next belong to.
    void set_slow_site_of(const BronzeInstruction& inst) {
        std::vector<uint32_t> sites;
        if (inst.slow_site != BronzeInstruction::kNoSlowSite) sites.push_back(inst.slow_site);
        prop_lowering_.set_slow_sites(std::move(sites));
    }
    const TranslatorOptions& options() const { return options_; }
    const BronzeModuleAST* current_ast() const { return current_ast_; }
    const BronzeFunction* current_fn_ast() const {
        return (current_ast_ && current_fn_idx_ < current_ast_->functions.size())
                   ? &current_ast_->functions[current_fn_idx_]
                   : nullptr;
    }
    BasicBlock* entry_hoist_block() const { return hoist_block_; }
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
    // Whether `id` is statically guaranteed to be a primitive value (never a
    // heap reference), so storing it requires no GC write barrier.
    bool is_primitive_value(uint32_t id) const { return primitive_value_ids_.count(id) != 0; }
    // Environment record access, inline (il_env_lowering.cpp): `depth`
    // parent hops from `env`, then slot `index`. The TDZ read calls
    // bronze_env_get_tdz only for the uninitialized marker, which throws.
    Value* lower_env_get(Builder& b, Value* env, uint32_t depth, uint32_t index);
    Value* lower_env_get_tdz(Builder& b, Value* env, uint32_t depth, uint32_t index, uint32_t key_idx);
    void lower_env_set(Builder& b, Value* env, uint32_t depth, uint32_t index, Value* val, bool val_is_primitive);

    // 0/1 values (il_lowering_bool.cpp): a result the dynamic compare and
    // ToBoolean lowerings merge from comparisons, noted where it is made, so
    // a branch on its negation can branch on it with its targets swapped.
    void note_boolean(Value* v) { if (v) boolean_values_.insert(v); }
    bool is_zero_one(const Value* v) const;
    // c when `cond` is `eq c, 0` of a 0/1 value c, else null.
    Value* negated_boolean(Value* cond) const;

private:
    // Per-function values computed once at the function's entry and shared
    // by every use (il_lowering_hoist.cpp): the inline-cache table's address.
    enum class Hoist : uint8_t { IcTable, Count };
    Value* hoisted(Builder& b, Hoist kind);
    // Starts a function's hoisting: values go into `block`, after whatever it
    // holds now.
    void begin_hoisting(BasicBlock* block);
    // The function's primitive and dynamic values (il_lowering_hoist.cpp).
    void analyze_function(const BronzeFunction& fn_ast);
    BasicBlock* hoist_block_ = nullptr;
    Instruction* hoist_after_ = nullptr;
    bool hoist_at_head_ = false;
    Value* hoisted_[static_cast<size_t>(Hoist::Count)] = {};
    std::unordered_set<uint32_t> primitive_value_ids_;
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
    // Sets `b`'s location to the instruction's: its own line in its own file.
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
    std::unordered_set<const Value*> boolean_values_;
};

} // namespace il2mir
