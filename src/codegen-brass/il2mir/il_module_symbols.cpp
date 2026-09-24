#include "il_abi.h"
#include <brass/mir/module.hpp>

namespace il2mir {

void register_all_module_external_symbols(Module* mod, const std::string& entry_symbol) {
    if (!mod) return;
    static const char* const kSymbols[] = {
        "bronze_print_f64", "bronze_print_i32", "bronze_print_dynamic", "bronze_print_space", "bronze_print_newline",
        "bronze_print_f64_err", "bronze_print_i32_err", "bronze_print_dynamic_err", "bronze_print_space_err",
        "bronze_print_newline_err", "bronze_print_spread", "bronze_print_spread_err", "bronze_immutable_assign",
        "bronze_dynamic_add", "bronze_f64_mod", "bronze_name_resolve", "bronze_resolve_name", "bronze_env_create",
        "bronze_env_get", "bronze_env_get_tdz", "bronze_env_set", "bronze_env_ancestor", "bronze_create_func",
        "bronze_create_function", "bronze_function_singleton", "bronze_import_meta", "bronze_create_array",
        "bronze_create_object", "bronze_prop_get", "bronze_prop_set", "bronze_elem_get", "bronze_elem_set",
        "bronze_method_def", "bronze_method_def_computed", "bronze_define_own_attr", "bronze_accessor_def",
        "bronze_accessor_def_computed", "bronze_module_namespace", "bronze_ic_get", "bronze_ic_set",
        "bronze_call_dynamic_0", "bronze_call_dynamic_1", "bronze_call_dynamic_2", "bronze_call_dynamic_3",
        "bronze_call_dynamic_4", "bronze_call_dynamic_5", "bronze_call_dynamic_6", "bronze_call_dynamic_7",
        "bronze_call_dynamic_8", "bronze_call_dynamic_9", "bronze_call_dynamic_10", "bronze_call_dynamic_11",
        "bronze_call_dynamic_12", "bronze_call_dynamic_13", "bronze_call_dynamic_14", "bronze_call_dynamic_15",
        "bronze_call_dynamic_16", "bronze_call_dynamic_n",
        "bronze_get_new_target", "bronze_bigint_literal", "bronze_async_machine",
        "bronze_async_start", "bronze_async_await", "bronze_to_string", "bronze_prop_delete", "bronze_elem_delete",
        "bronze_iter_open", "bronze_iter_step",
        "brass_coro_create", "brass_coro_resume", "brass_coro_is_done", "brass_coro_destroy",
        "bronze_tls_block_addr", "__bronze_key_map", "__bronze_template_cells", "bronze_template_object",
        "bronze_register_value_cells", "bronze_register_fn_sources", "bronze_concat_begin", "bronze_concat_append",
        "bronze_concat_end", "bronze_global_get_name", "bronze_global_get", "bronze_typeof",
        "bronze_construct_0", "bronze_construct_1", "bronze_construct_2", "bronze_construct_3", "bronze_construct_4",
        "bronze_construct_5", "bronze_construct_6", "bronze_construct_7", "bronze_construct_8", "bronze_construct_9",
        "bronze_construct_10", "bronze_construct_11", "bronze_construct_12", "bronze_construct_13",
        "bronze_construct_14", "bronze_construct_15", "bronze_construct_16", "bronze_construct",
        "bronze_construct_n", "bronze_class_extends", "bronze_super_call",
        "bronze_super_call_0", "bronze_super_call_1", "bronze_super_call_2", "bronze_super_call_3",
        "bronze_super_call_4", "bronze_super_call_5", "bronze_super_call_6", "bronze_super_call_7",
        "bronze_super_call_8", "bronze_super_call_9", "bronze_super_call_10", "bronze_super_call_11",
        "bronze_super_call_12", "bronze_super_call_13", "bronze_super_call_14", "bronze_super_call_15",
        "bronze_super_call_16", "bronze_super_call_n",
        "bronze_create_generator_object", "bronze_create_async_generator_object",
        "bronze_dynamic_import", "bronze_iter_value", "bronze_iter_close", "bronze_iter_rest",
        "bronze_iter_delegate", "bronze_async_iter_open", "bronze_async_iter_next", "bronze_async_iter_close",
        "bronze_pattern_check", "bronze_array_append", "bronze_array_append_hole", "bronze_array_spread",
        "bronze_object_spread", "bronze_object_rest", "bronze_dynamic_call_spread", "bronze_call_method_spread",
        "bronze_call_method", "bronze_register_method_ic_cells", "__bronze_ic_table", "__bronze_method_ic_sites",
        "bronze_construct_spread", "bronze_super_call_spread", "bronze_arg_at", "bronze_arguments_object",
        "bronze_rest_args", "bronze_super_get", "bronze_super_set", "bronze_object_keys", "bronze_for_in_keys",
        "bronze_instanceof", "bronze_has_property", "bronze_is_nullish", "bronze_strict_eq", "bronze_loose_eq",
        "bronze_rel_lt", "bronze_rel_gt", "bronze_rel_le", "bronze_rel_ge", "bronze_pin_guard",
        "bronze_census_record", "bronze_to_int32", "bronze_to_int32_f64", "bronze_private_new", "bronze_private_has",
        "bronze_private_get", "bronze_private_add", "bronze_private_set", "bronze_private_misuse",
        "bronze_census_register", "__bronze_census_out_path", "__bronze_census_sites",
        "bronze_register_key_manifest", "bronze_box_str_key", "bronze_box_str", "bronze_unbox_str",
        "bronze_unbox_f64", "bronze_box_f64", "bronze_unbox_i32", "bronze_box_i32", "bronze_unbox_bool",
        "bronze_box_bool", "bronze_exception_get", "bronze_exception_set", "bronze_exception_take",
        "bronze_exception_pending", "bronze_uncaught_exception", "bronze_gc_frame_push", "bronze_gc_frame_pop",
        "bronze_tls_enter", "bronze_stack_overflow", "bronze_pin_violation", "bronze_pin_check_array", "bronze_pow",
        "bronze_dynamic_pow", "bronze_dynamic_bitand", "bronze_dynamic_bitor", "bronze_dynamic_bitxor",
        "bronze_dynamic_shl", "bronze_dynamic_shr", "bronze_dynamic_ushr", "bronze_dynamic_sub",
        "bronze_dynamic_mul", "bronze_dynamic_div", "bronze_dynamic_mod", "bronze_dynamic_neg",
        "bronze_dynamic_bitnot",
        "sin", "cos", "sqrt", "fabs", "floor", "ceil", "trunc",
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
    // JS `%` on numbers: fmod.
    mod->add_symbol_role("bronze_f64_mod", SymbolRole::FloatRem);
    const std::string key_sym = (entry_symbol.empty() || entry_symbol == "main" || entry_symbol == "bronze_main")
                                    ? "bronze_main_key_constants"
                                    : (entry_symbol + "_key_constants");
    mod->add_external_symbol(key_sym);

    if (!entry_symbol.empty() && entry_symbol != "main" && entry_symbol != "bronze_main") {
        mod->add_external_symbol("__bronze_module_env_" + entry_symbol);
        mod->add_external_symbol("__bronze_key_map_" + entry_symbol);
        mod->add_external_symbol("__bronze_template_cells_" + entry_symbol);
        mod->add_external_symbol("__bronze_census_out_path_" + entry_symbol);
        mod->add_external_symbol("__bronze_census_sites_" + entry_symbol);
        mod->add_external_symbol("__bronze_ic_table_" + entry_symbol);
        mod->add_external_symbol("__bronze_method_ic_sites_" + entry_symbol);
    }
}

} // namespace il2mir
