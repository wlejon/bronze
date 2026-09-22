add_library(bronze_cli STATIC driver.cpp link.cpp)
add_library(bronze::cli ALIAS bronze_cli)
target_include_directories(bronze_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(bronze_cli PUBLIC bronze::parse bronze::lex bronze::ast bronze::support bronze::il bronze::types bronze::lower bronze::codegen bronze::modules bronze::codegen_brass)
add_dependencies(bronze_cli bronze_rt)

if(MSVC)
    target_compile_options(bronze_cli PRIVATE /W4 /WX /permissive- /wd4324)
    target_compile_definitions(bronze_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(bronze main.cpp run.cpp)
target_link_libraries(bronze PRIVATE bronze::cli bronze::eval)
if(MSVC)
    target_compile_options(bronze PRIVATE /W4 /WX /permissive- /wd4324)
    target_compile_definitions(bronze PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(bronze PRIVATE /FORCE:MULTIPLE)
    # 8 MB main-thread stack, like bro's: the 1 MB default caps JS recursion
    # near 7,000 frames before the runtime's stack-limit check turns it
    # into a RangeError.
    target_link_options(bronze PRIVATE /STACK:8388608)
endif()

# Hand-rolled rather than through bronze_add_module (the library above is not
# one), so the BRONZE_BUILD_TESTS guard has to be written out here too.
if(BRONZE_BUILD_TESTS)
    add_executable(bronze_cli_tests ../../tests/cli/cli_test.cpp
                                   ../../tests/cli/performance_now_test.cpp
                                   ../../tests/cli/pin_barrier_test.cpp
                                   ../../tests/cli/pin_census_test.cpp
                                   ../../tests/cli/numeric_arith_boundary_test.cpp
                                   ../../tests/cli/lowering_determinism_test.cpp
                                   ../../tests/cli/large_arg_call_test.cpp)
    target_link_libraries(bronze_cli_tests PRIVATE bronze::cli doctest::doctest)
    # The determinism test needs a graph big enough for the hazard to have room
    # to happen in, and the vendored three.js one is already in the tree.
    target_compile_definitions(bronze_cli_tests PRIVATE
        TEST_THREEJS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/oracle/threejs")
    if(MSVC)
        target_compile_options(bronze_cli_tests PRIVATE /W4 /WX /permissive-)
        target_compile_definitions(bronze_cli_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_options(bronze_cli_tests PRIVATE /FORCE:MULTIPLE)
    endif()
    add_test(NAME cli COMMAND bronze_cli_tests)
    set_tests_properties(cli PROPERTIES LABELS cli)
endif()
