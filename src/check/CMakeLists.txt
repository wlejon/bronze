bronze_add_module(check
    SOURCES
        static_checker.cpp
        static_checker_math.cpp
        static_checker_domain.cpp
        prove_equiv.cpp
        prove_equiv_heap.cpp
        prove_equiv_builtins.cpp
        prove_equiv_bitwise.cpp
        prove_equiv_calls.cpp
        prove_loop_induction.cpp
        prove_loop_relational.cpp
        loop_recurrence_synth.cpp
        deterministic_check.cpp
        synth_tests.cpp
        solve_fn.cpp
        cegis_optimize.cpp
        cegis_dag_enum.cpp
        cegis_superopt.cpp
        js_source_rewriter.cpp
        js_rewrite_ast.cpp
        js_rewrite_passes.cpp
        js_subexpr_superopt.cpp
        fuzz_fn.cpp
        prove_fn.cpp
        layout_solver.cpp
    DEPS ast il lower modules support types
    TEST_SOURCES
        ../../tests/check/static_checker_test.cpp
        ../../tests/check/prover_tools_test.cpp
        ../../tests/check/symbolic_runtime_test.cpp
        ../../tests/check/verified_rewriter_test.cpp
        ../../tests/check/loop_fp_prover_test.cpp
        ../../tests/check/threejs_math_prover_test.cpp
    TEST_DEPS lex parse
)

if(DEFINED BRASS_ROOT AND EXISTS "${BRASS_ROOT}/include")
    target_include_directories(bronze_check PUBLIC "${BRASS_ROOT}/include" "${BRASS_ROOT}/src")
endif()
if(TARGET brass)
    target_link_libraries(bronze_check PUBLIC brass)
endif()
