#pragma once

#include "il_ast.h"
#include "il_translator.h"
#include "il_property.h"
#include "il_alloc_lowering.h"
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

class IlLowering {
public:
    IlLowering(const TranslatorOptions& options, DiagnosticReporter* diag = nullptr);

    std::unique_ptr<Module> lower_module(const BronzeModuleAST& ast);
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
    const TranslatorOptions& options() const { return options_; }
    const BronzeModuleAST* current_ast() const { return current_ast_; }
    DiagnosticReporter* diag() const { return diag_; }
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

private:
    bool lower_function(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name);
    bool emit_wrapper(const BronzeFunction& fn_ast, Module& mod, const std::string& fn_name, uint32_t declared_param_count, bool is_closure = false);
    bool lower_instruction(const BronzeInstruction& inst_ast, Builder& b, Function* fn,
                           std::unordered_map<uint32_t, Value*>& val_map,
                           const std::unordered_map<uint32_t, BasicBlock*>& block_map,
                           uint32_t handler_id = UINT32_MAX,
                           uint32_t block_id = 0,
                           uint32_t* cont_counter = nullptr);

    TranslatorOptions options_;
    DiagnosticReporter* diag_ = nullptr;
    PropertyLoweringHelper prop_lowering_;
    AllocLoweringHelper alloc_lowering_;
    uint32_t current_file_id_ = 0;
    // The debug-context file id of each BronzeModuleAST::source_files entry.
    std::vector<uint32_t> source_file_ids_;
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
};

} // namespace il2mir
