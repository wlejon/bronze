bronze_add_module(lower
    SOURCES lower.cpp lower_infer.cpp infer_stats.cpp lower_util.cpp lower_scope.cpp lower_stmt.cpp
            lower_class.cpp lower_control.cpp lower_expr.cpp lower_expr_assign.cpp lower_expr_binary.cpp lower_expr_cond.cpp lower_generator.cpp
            lower_update.cpp lower_private.cpp lower_typed_elem.cpp bigint_reach.cpp
            lower_object.cpp lower_call.cpp lower_pattern.cpp
            lower_iter_loop.cpp lower_switch.cpp lower_label.cpp lower_expr_chain.cpp
            lower_try.cpp lower_unresolved.cpp lower_yield_star.cpp lower_async.cpp
    DEPS ast il support types
    TEST_SOURCES ../../tests/lower/lower_test.cpp ../../tests/lower/lower_jumps_test.cpp
                 ../../tests/lower/lower_try_test.cpp ../../tests/lower/lower_infer_test.cpp
                 ../../tests/lower/lower_scope_test.cpp
                 ../../tests/lower/lower_for_env_test.cpp
                 ../../tests/lower/lower_generator_test.cpp
                 ../../tests/lower/lower_loop_exit_test.cpp
                 ../../tests/lower/lower_unresolved_test.cpp
                 ../../tests/lower/lower_strict_test.cpp
                 ../../tests/lower/lower_env_slot_test.cpp
                 ../../tests/lower/lower_direct_method_test.cpp
                 ../../tests/lower/lower_stable_closure_call_test.cpp
    # Test-only: the fixtures need a lexer and parser to produce an AST.
    # `bronze::lower` itself consumes an AST it is handed and includes
    # neither (see the note on TEST_DEPS in cmake/bronze_module.cmake).
    TEST_DEPS lex parse
)

# GCC 13's -Wmaybe-uninitialized cannot see through std::optional's engaged
# flag when the generator context is saved/restored by move (GCC bug 80635
# family) and reports the payload members as maybe-uninitialized under
# -Werror. False positive — every member has a default initializer. Scoped to
# the one file with the save/restore rather than losing the warning
# everywhere; GCC-only, so clang and MSVC still watch this file.
set_source_files_properties(lower_generator.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>")
