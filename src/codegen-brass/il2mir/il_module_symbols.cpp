#include "il_abi.h"
#include <brass/mir/module.hpp>

namespace il2mir {

// The object emitter's rule (brass_backend_sections.cpp): only the default
// entry, `bronze_main`, gets the unsuffixed `bronze_object_*` names.
std::string code_ranges_symbol(const std::string& entry_symbol) {
    return entry_symbol == "bronze_main" ? "bronze_object_code_ranges" : entry_symbol + "_code_ranges";
}

std::string code_range_count_symbol(const std::string& entry_symbol) {
    return entry_symbol == "bronze_main" ? "bronze_object_code_range_count"
                                         : entry_symbol + "_code_range_count";
}

void register_all_module_external_symbols(Module* mod, const std::string& entry_symbol) {
    if (!mod) return;
    static const char* const kSymbols[] = {
        "bronze_print_f64", "bronze_print_i32", "bronze_print_dynamic", "bronze_print_space", "bronze_print_newline",
        "bronze_print_f64_err", "bronze_print_i32_err", "bronze_print_dynamic_err", "bronze_print_space_err",
        "bronze_print_newline_err", "bronze_print_spread", "bronze_print_spread_err", "bronze_immutable_assign",
        "bronze_dynamic_add", "bronze_f64_mod", "bronze_name_resolve", "bronze_resolve_name", "bronze_env_create",
        "bronze_env_get", "bronze_env_get_tdz", "bronze_env_set", "bronze_env_ancestor", "bronze_create_func",
        "bronze_create_function", "bronze_function_singleton", "bronze_import_meta", "bronze_create_array",
        "bronze_create_object", "bronze_create_object_sized", "bronze_create_this", "bronze_alloc_pretenured", "bronze_gc_alloc","bronze_prop_get", "bronze_prop_set", "bronze_elem_get", "bronze_elem_set",
        "bronze_prop_define", "bronze_elem_define", "bronze_object_literal_proto",
        "bronze_method_def", "bronze_method_def_computed", "bronze_define_own_attr", "bronze_accessor_def",
        "bronze_accessor_def_computed", "bronze_module_namespace", "bronze_ic_get", "bronze_ic_set", "bronze_call_dynamic_n",
        "bronze_get_new_target", "bronze_bigint_literal", "bronze_coro_start", "bronze_to_string", "bronze_prop_delete", "bronze_elem_delete",
        "bronze_iter_open", "bronze_iter_step",
        "bronze_tls_block_addr", "__bronze_key_map", "__bronze_template_cells", "bronze_template_object",
        "bronze_register_value_cells", "bronze_register_template_cells", "bronze_register_fn_sources",
        "bronze_register_code_ranges",
        "bronze_concat_begin", "bronze_concat_append", "bronze_concat_template",
        "bronze_concat_end", "bronze_global_get_name", "bronze_global_get", "bronze_typeof", "bronze_is_callable", "bronze_array_push", "bronze_array_push1", "bronze_collection_op", "bronze_construct", "bronze_class_extends", "bronze_super_call", "bronze_super_call_n",
        "bronze_dynamic_import", "bronze_iter_value", "bronze_iter_close", "bronze_iter_rest",
        "bronze_iter_delegate", "bronze_async_iter_open", "bronze_async_iter_next", "bronze_async_iter_close",
        "bronze_pattern_check", "bronze_array_append", "bronze_array_spread",
        "bronze_object_spread", "bronze_object_rest", "bronze_dynamic_call_spread", "bronze_call_method_spread",
        "bronze_call_method", "bronze_register_method_ic_cells", "__bronze_ic_table", "__bronze_method_ic_sites",
        "bronze_register_shape_cells", "__bronze_shape_cells", "__bronze_shape_cell_keys",
        "bronze_register_slot_repr", "__bronze_number_slot_keys",
        "__bronze_literal_shapes", "bronze_object_literal", "__bronze_proto_cells", "bronze_transition_publish",
        "__bronze_member_cells", "bronze_member_cell_publish", "__bronze_deopt_counts", "__bronze_cold_entries",
        "__bronze_elem_cells", "bronze_elem_get_cell",
        "bronze_register_deopt_counts",
        "bronze_register_ic_hot", "bronze_register_method_ic_rows", "__bronze_ic_identity",
        "bronze_construct_spread", "bronze_super_call_spread", "bronze_arg_at", "bronze_arguments_object",
        "bronze_rest_args", "bronze_super_get", "bronze_super_set", "bronze_for_in_keys", "bronze_for_in_live",
        "bronze_instanceof", "bronze_has_property", "bronze_is_nullish", "bronze_strict_eq", "bronze_loose_eq",
        "bronze_rel_lt", "bronze_rel_gt", "bronze_rel_le", "bronze_rel_ge", "bronze_pin_guard",
        "bronze_census_record", "bronze_to_int32", "bronze_to_int32_f64", "bronze_private_new", "bronze_private_has",
        "bronze_private_get", "bronze_private_add", "bronze_private_set", "bronze_private_misuse",
        "bronze_census_register", "__bronze_census_out_path", "__bronze_census_sites",
        "bronze_register_key_manifest", "bronze_box_str_key", "bronze_box_str", "bronze_unbox_str",
        "bronze_unbox_f64", "bronze_box_f64", "bronze_unbox_i32", "bronze_box_i32", "bronze_unbox_bool",
        "bronze_box_bool",
        "bronze_tls_enter", "bronze_stack_overflow", "bronze_interrupt_poll", "bronze_pin_violation", "bronze_pin_check_array", "bronze_pow",
        "bronze_dynamic_pow", "bronze_dynamic_bitand", "bronze_dynamic_bitor", "bronze_dynamic_bitxor",
        "bronze_dynamic_shl", "bronze_dynamic_shr", "bronze_dynamic_ushr", "bronze_dynamic_sub",
        "bronze_dynamic_mul", "bronze_dynamic_div", "bronze_dynamic_mod", "bronze_dynamic_neg",
        "bronze_dynamic_bitnot",
        "sin", "cos", "sqrt", "fabs", "floor", "ceil", "trunc", "brass_sin_f64", "brass_cos_f64",
        "brass_sincos_f64", "bronze_math_random_f64",
    };
    for (const char* sym : kSymbols) {
        mod->add_external_symbol(sym);
    }
    // The runtime contracts the optimizer relies on (brass's
    // runtime_symbols.hpp). bronze_tls_block_addr returns the calling
    // thread's TLS block: no side effect, no GC, the same answer every time.
    mod->add_symbol_role("bronze_tls_block_addr", SymbolRole::Pure);
    mod->add_symbol_role("bronze_create_array", SymbolRole::ArrayNew);
    mod->add_symbol_role("bronze_elem_get", SymbolRole::ArrayGet);
    mod->add_symbol_role("bronze_elem_set", SymbolRole::ArraySet);
    // JS `%` on numbers: fmod, pure with respect to memory and GC.
    mod->add_symbol_role("bronze_f64_mod", SymbolRole::FloatRem);
    mod->add_symbol_role("bronze_f64_mod", SymbolRole::Pure);
    mod->add_symbol_role("bronze_pow", SymbolRole::Pure);
    // Math.sin / Math.cos's kernels (brass's trig runtime, V8's bits)...
    mod->add_symbol_role("brass_sin_f64", SymbolRole::Pure);
    mod->add_symbol_role("brass_cos_f64", SymbolRole::Pure);
    // The libm and numeric math members' kernels (builtin_math.cpp): plain
    // functions of their arguments, read nothing, write nothing, never collect.
    for (const char* sym : {"bronze_math_exp_f64", "bronze_math_log_f64", "bronze_math_tan_f64",
                            "bronze_math_asin_f64", "bronze_math_acos_f64", "bronze_math_atan_f64",
                            "bronze_math_atan2_f64", "bronze_math_round_f64", "bronze_math_min2_f64",
                            "bronze_math_max2_f64"}) {
        mod->add_external_symbol(sym);
        mod->add_symbol_role(sym, SymbolRole::Pure);
    }
    // ...and their fused form, the same bits from one reduction (brass's
    // sincos_fuse pairs a sine and a cosine of one value into it). It writes
    // only its out slot and never collects.
    mod->add_symbol_role("brass_sin_f64", SymbolRole::SinF64);
    mod->add_symbol_role("brass_cos_f64", SymbolRole::CosF64);
    mod->add_symbol_role("brass_sincos_f64", SymbolRole::SinCosF64);
    mod->add_symbol_role("brass_sincos_f64", SymbolRole::NoCollect);
    // Math.random's generator and leaf query/claim helpers touch no GC-traced
    // heap allocation, so none of them can trigger a collection.
    for (const char* sym : {"bronze_math_random_f64", "bronze_strict_eq", "bronze_is_callable",
                            "bronze_array_claim_numbers", "bronze_shape_slot_keys", "bronze_shape_lacks"}) {
        mod->add_external_symbol(sym);
        mod->add_symbol_role(sym, SymbolRole::NoCollect);
    }
    const std::string key_sym = (entry_symbol.empty() || entry_symbol == "main" || entry_symbol == "bronze_main")
                                    ? "bronze_main_key_constants"
                                    : (entry_symbol + "_key_constants");
    mod->add_external_symbol(key_sym);
    mod->add_external_symbol(code_ranges_symbol(entry_symbol));
    mod->add_external_symbol(code_range_count_symbol(entry_symbol));

    if (!entry_symbol.empty() && entry_symbol != "main" && entry_symbol != "bronze_main") {
        mod->add_external_symbol("__bronze_module_env_" + entry_symbol);
        mod->add_external_symbol("__bronze_key_map_" + entry_symbol);
        mod->add_external_symbol("__bronze_template_cells_" + entry_symbol);
        mod->add_external_symbol("__bronze_census_out_path_" + entry_symbol);
        mod->add_external_symbol("__bronze_census_sites_" + entry_symbol);
        mod->add_external_symbol("__bronze_ic_table_" + entry_symbol);
        mod->add_external_symbol("__bronze_method_ic_sites_" + entry_symbol);
        mod->add_external_symbol("__bronze_ic_identity_" + entry_symbol);
    }
}

} // namespace il2mir
