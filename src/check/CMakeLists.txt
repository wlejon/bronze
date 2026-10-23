bronze_add_module(check
    SOURCES
        static_checker.cpp
        static_checker_math.cpp
        static_checker_domain.cpp
        applet_cert.cpp
        prove_equiv.cpp
        prove_equiv_bitwise.cpp
        deterministic_check.cpp
        synth_tests.cpp
        solve_fn.cpp
        cegis_optimize.cpp
        cegis_superopt.cpp
        js_source_rewriter.cpp
        auto_kernel_rewriter.cpp
        live_supervisor.cpp
        fuzz_fn.cpp
        prove_fn.cpp
        layout_solver.cpp
    DEPS ast il lower modules support types
    TEST_SOURCES
        ../../tests/check/static_checker_test.cpp
        ../../tests/check/prover_tools_test.cpp
        ../../tests/check/applet_cert_test.cpp
        ../../tests/check/symbolic_runtime_test.cpp
        ../../tests/check/verified_rewriter_test.cpp
    TEST_DEPS lex parse
)

if(DEFINED BRASS_ROOT AND EXISTS "${BRASS_ROOT}/include")
    target_include_directories(bronze_check PUBLIC "${BRASS_ROOT}/include" "${BRASS_ROOT}/src")
endif()
if(TARGET brass)
    target_link_libraries(bronze_check PUBLIC brass)
endif()
