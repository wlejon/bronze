add_library(bronze_cli STATIC driver.cpp)
add_library(bronze::cli ALIAS bronze_cli)
target_include_directories(bronze_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(bronze_cli PUBLIC bronze::parse bronze::lex bronze::ast bronze::support bronze::il bronze::types bronze::lower bronze::codegen bronze::modules)
add_dependencies(bronze_cli bronze_rt)

if(BRONZE_WITH_LLVM)
    target_compile_definitions(bronze_cli PUBLIC BRONZE_WITH_LLVM)
    target_link_libraries(bronze_cli PUBLIC bronze::codegen_llvm)
endif()

if(MSVC)
    target_compile_options(bronze_cli PRIVATE /W4 /WX /permissive-)
endif()

add_executable(bronze main.cpp)
target_link_libraries(bronze PRIVATE bronze::cli)
if(MSVC)
    target_compile_options(bronze PRIVATE /W4 /WX /permissive-)
endif()

add_executable(bronze_cli_tests ../../tests/cli/cli_test.cpp)
target_link_libraries(bronze_cli_tests PRIVATE bronze::cli doctest::doctest)
if(MSVC)
    target_compile_options(bronze_cli_tests PRIVATE /W4 /WX /permissive-)
endif()
add_test(NAME cli COMMAND bronze_cli_tests)
set_tests_properties(cli PROPERTIES LABELS cli)
