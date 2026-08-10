# bronze_add_module(<name> SOURCES ... [DEPS ...] [TEST_SOURCES ...])
#
# Every compiler component is an isolated STATIC library with its own test
# executable. Nothing links more than it declares; the CLI is the only place
# modules meet. Tests register under a ctest label matching the module name,
# so the iteration loop is always scoped:
#
#     cmake --build --preset dev --target bronze_lex_tests
#     ctest --preset dev -L lex
#
function(bronze_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;TEST_SOURCES" ${ARGN})

    add_library(bronze_${name} STATIC ${ARG_SOURCES})
    add_library(bronze::${name} ALIAS bronze_${name})
    target_include_directories(bronze_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(MSVC)
        target_compile_options(bronze_${name} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(bronze_${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(bronze_${name} PUBLIC bronze::${dep})
    endforeach()

    if(ARG_TEST_SOURCES)
        add_executable(bronze_${name}_tests ${ARG_TEST_SOURCES})
        target_link_libraries(bronze_${name}_tests PRIVATE bronze::${name} doctest::doctest)
        if(MSVC)
            target_compile_options(bronze_${name}_tests PRIVATE /W4 /WX /permissive-)
        endif()
        add_test(NAME ${name} COMMAND bronze_${name}_tests)
        set_tests_properties(${name} PROPERTIES LABELS ${name})
    endif()
endfunction()
