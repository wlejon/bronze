add_executable(bronze_oracle_tests oracle_test.cpp)
target_include_directories(bronze_oracle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(bronze_oracle_tests PRIVATE
    TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases"
    TEST_THREEJS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/threejs"
    TEST_PIXI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pixi"
    TEST_BRONZE_CLI="$<TARGET_FILE:bronze>")

target_link_libraries(bronze_oracle_tests PRIVATE bronze::cli doctest::doctest)
# The JIT half runs every case through the CLI's `bronze run`, so the CLI is
# part of what the harness tests.
add_dependencies(bronze_oracle_tests bronze)

if(MSVC)
    target_compile_options(bronze_oracle_tests PRIVATE /W4 /WX /permissive-)
    target_link_options(bronze_oracle_tests PRIVATE /FORCE:MULTIPLE)
endif()

# The suites share one binary and are split into ctest tests by doctest's own
# name filter, so that the ~70 s three.js compile can be selected or skipped
# on its own without the case list forking into a second harness. The filters
# are case-INSENSITIVE and anchored at the start of the name: `*jit*` would
# also select the two `-jit` milestones.
set(BRONZE_ORACLE_CASE_SUITES --test-case-exclude=threejs*,pixi*,Oracle\ JIT*)

# The cases are compiled and run both with and without inference, and re-run
# under BRONZE_GC_STRESS=1 on the same built executable to verify rooting without
# a duplicate compilation pass.
add_test(NAME oracle COMMAND bronze_oracle_tests ${BRONZE_ORACLE_CASE_SUITES})
set_tests_properties(oracle PROPERTIES LABELS oracle)

# The same cases through `bronze run` — the in-process JIT path the engine
# uses — held to the same pinned bytes, and to the built program's exit code
# and uncaught-error report. Its own test so the two halves show up as two
# results and can be selected apart (`-L jit`).
add_test(NAME oracle-jit COMMAND bronze_oracle_tests "--test-case=Oracle JIT*")
set_tests_properties(oracle-jit PROPERTIES LABELS "oracle;jit")

# Unmodified three.js r160 (tests/oracle/threejs/README.md). Its own label
# because it costs more than the whole rest of the suite: `-L threejs` runs
# only the milestone, `-LE threejs` is the fast loop. It carries its own
# gc-stress run internally rather than being listed twice, since the second
# run reuses the executable the first one built.
add_test(NAME oracle-threejs COMMAND bronze_oracle_tests "--test-case=threejs milestone*")
set_tests_properties(oracle-threejs PROPERTIES LABELS threejs)

# Unmodified pixi.js v8.19.0 (tests/oracle/pixi/README.md), under its own
# label for the same reason as three.js: the bundle costs a whole-library
# compile per inference mode, so `-L pixi` selects it and `-LE "threejs|pixi"`
# is the fast loop when both milestones should stay out of the way.
add_test(NAME oracle-pixi COMMAND bronze_oracle_tests "--test-case=pixi milestone*")
set_tests_properties(oracle-pixi PROPERTIES LABELS pixi)

# The milestones through `bronze run`: the whole library JIT-compiled in one
# process. Under the milestone labels, so the fast loop's `-LE "threejs|pixi"`
# leaves them out, and under `jit-milestone` to select just these two.
add_test(NAME oracle-threejs-jit COMMAND bronze_oracle_tests "--test-case=threejs-jit*")
set_tests_properties(oracle-threejs-jit PROPERTIES LABELS "threejs;jit-milestone")
add_test(NAME oracle-pixi-jit COMMAND bronze_oracle_tests "--test-case=pixi-jit*")
set_tests_properties(oracle-pixi-jit PROPERTIES LABELS "pixi;jit-milestone")
