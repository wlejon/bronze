add_executable(bronze_oracle_tests oracle_test.cpp)
target_include_directories(bronze_oracle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(bronze_oracle_tests PRIVATE TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases")

target_link_libraries(bronze_oracle_tests PRIVATE bronze::cli doctest::doctest)

if(MSVC)
    target_compile_options(bronze_oracle_tests PRIVATE /W4 /WX /permissive-)
endif()

add_test(NAME oracle COMMAND bronze_oracle_tests)
set_tests_properties(oracle PROPERTIES LABELS oracle)

# The same cases again with per-allocation collection forced on. Compiled
# cases inherit the environment, so every allocation inside every case
# moves the entire live set — a missing root in generated code becomes a
# deterministic byte mismatch rather than a bug with a timer on it
# (docs/0006 decision 5).
add_test(NAME oracle-gc-stress COMMAND bronze_oracle_tests)
set_tests_properties(oracle-gc-stress PROPERTIES
    LABELS oracle
    ENVIRONMENT "BRONZE_GC_STRESS=1")
