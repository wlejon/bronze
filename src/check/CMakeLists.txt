bronze_add_module(check
    SOURCES
        static_checker.cpp
        static_checker_domain.cpp
        prove_equiv.cpp
        deterministic_check.cpp
        synth_tests.cpp
    DEPS ast il lower modules support types
    TEST_SOURCES
        ../../tests/check/static_checker_test.cpp
        ../../tests/check/prover_tools_test.cpp
    TEST_DEPS lex parse
)

if(DEFINED BRASS_ROOT AND EXISTS "${BRASS_ROOT}/include")
    target_include_directories(bronze_check PUBLIC "${BRASS_ROOT}/include" "${BRASS_ROOT}/src")
endif()
if(TARGET brass)
    target_link_libraries(bronze_check PUBLIC brass)
endif()
