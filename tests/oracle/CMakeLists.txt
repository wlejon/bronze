add_executable(bronze_oracle_tests oracle_test.cpp)
target_include_directories(bronze_oracle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_definitions(bronze_oracle_tests PRIVATE TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cases")

target_link_libraries(bronze_oracle_tests PRIVATE bronze::cli doctest::doctest)

if(MSVC)
    target_compile_options(bronze_oracle_tests PRIVATE /W4 /WX /permissive-)
endif()

add_test(NAME oracle COMMAND bronze_oracle_tests)
set_tests_properties(oracle PROPERTIES LABELS oracle)
