add_executable(bronze_oracle_tests oracle_test.cpp)
target_include_directories(bronze_oracle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(bronze_oracle_tests PRIVATE
    TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases"
    TEST_THREEJS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/threejs")

target_link_libraries(bronze_oracle_tests PRIVATE bronze::cli doctest::doctest)

if(MSVC)
    target_compile_options(bronze_oracle_tests PRIVATE /W4 /WX /permissive-)
endif()

# The three suites share one binary and are split into three ctest tests by
# doctest's own name filter, so that the ~70 s three.js compile can be selected
# or skipped on its own without the case list forking into a second harness.
set(BRONZE_ORACLE_CASE_SUITES --test-case-exclude=threejs*)

add_test(NAME oracle COMMAND bronze_oracle_tests ${BRONZE_ORACLE_CASE_SUITES})
set_tests_properties(oracle PROPERTIES LABELS oracle)

# The same cases again with per-allocation collection forced on. Compiled
# cases inherit the environment, so every allocation inside every case
# moves the entire live set — a missing root in generated code becomes a
# deterministic byte mismatch rather than a bug with a timer on it
# (docs/0006 decision 5).
add_test(NAME oracle-gc-stress COMMAND bronze_oracle_tests ${BRONZE_ORACLE_CASE_SUITES})
set_tests_properties(oracle-gc-stress PROPERTIES
    LABELS oracle
    ENVIRONMENT "BRONZE_GC_STRESS=1")

# Unmodified three.js r160 (tests/oracle/threejs/README.md). Its own label
# because it costs more than the whole rest of the suite: `-L threejs` runs
# only the milestone, `-LE threejs` is the fast loop. It carries its own
# gc-stress run internally rather than being listed twice, since the second
# run reuses the executable the first one built.
add_test(NAME oracle-threejs COMMAND bronze_oracle_tests --test-case=threejs*)
set_tests_properties(oracle-threejs PROPERTIES LABELS threejs)
