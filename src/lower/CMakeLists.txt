bronze_add_module(lower
    SOURCES lower.cpp lower_annotation.cpp lower_util.cpp lower_stmt.cpp
            lower_scope.cpp lower_closure.cpp
            lower_static_call_plan.cpp
            lower_class.cpp lower_control.cpp lower_expr.cpp lower_expr_assign.cpp lower_expr_binary.cpp lower_expr_cond.cpp lower_generator.cpp
            lower_update.cpp lower_private.cpp lower_class_fields.cpp lower_derived_ctor.cpp derived_ctor_plan.cpp
            lower_concat_chain.cpp lower_define_props.cpp
            lower_object.cpp lower_call.cpp
            lower_pattern.cpp lower_final_passes.cpp native_manifest.cpp lower_native.cpp
            lower_iter_loop.cpp lower_switch.cpp lower_label.cpp lower_expr_chain.cpp
            lower_try.cpp lower_unresolved.cpp lower_yield_star.cpp lower_async.cpp
            lower_segment.cpp lower_function.cpp lower_selfhost.cpp selfhost_merge.cpp
    DEPS ast il support types json
)

# GCC 13's -Wmaybe-uninitialized cannot see through std::optional's engaged
# flag when the generator context is saved/restored by move (GCC bug 80635
# family) and reports the payload members as maybe-uninitialized under
# -Werror. False positive — every member has a default initializer. Scoped to
# the one file with the save/restore rather than losing the warning
# everywhere; GCC-only, so clang and MSVC still watch this file.
set_source_files_properties(lower_generator.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>")
