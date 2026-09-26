# How fast real programs reach optimized code under `bronze run` (cpu_work,
# a top-level allocation loop, three.js, pixi), against pinned budgets:
# tier_ratchet.cpp says what is measured and holds the baseline. A
# performance target, not correctness: labelled "perf", as brass labels its
# ratchets, so `-LE perf` leaves it out.
add_executable(bronze_tier_ratchet tier_ratchet.cpp)
target_compile_definitions(bronze_tier_ratchet PRIVATE
    TEST_TIER_RATCHET_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    TEST_ORACLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../oracle"
    TEST_BRONZE_CLI="$<TARGET_FILE:bronze>")
add_dependencies(bronze_tier_ratchet bronze)
if(MSVC)
    target_compile_options(bronze_tier_ratchet PRIVATE /W4 /WX /permissive-)
endif()
add_test(NAME tier-ratchet COMMAND bronze_tier_ratchet)
set_tests_properties(tier-ratchet PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE LABELS "perf")
