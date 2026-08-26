# bronze_add_module(<name> SOURCES ... [DEPS ...] [TEST_SOURCES ...] [TEST_DEPS ...])
#
# TEST_DEPS links extra modules into the TEST binary only, for building
# fixtures. It deliberately does not touch the library's own dependencies:
# a module that analyses the AST needs a parser to get one in a test, and
# that must not become a dependency of the module itself.
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
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;TEST_SOURCES;TEST_DEPS" ${ARGN})

    add_library(bronze_${name} STATIC ${ARG_SOURCES})
    add_library(bronze::${name} ALIAS bronze_${name})
    target_include_directories(bronze_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(MSVC)
        target_compile_options(bronze_${name} PRIVATE /W4 /WX /permissive-)
        # The CRT-deprecation opt-out (getenv and friends are standard C++),
        # not a blanket C4996 disable. Target-wide because a per-file #define
        # placed after the first include is silently too late.
        target_compile_definitions(bronze_${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(bronze_${name} PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(bronze_${name} PRIVATE -Wno-restrict)
        endif()
    endif()
    foreach(dep IN LISTS ARG_DEPS)
        target_link_libraries(bronze_${name} PUBLIC bronze::${dep})
    endforeach()

    # BRONZE_BUILD_TESTS is off when bronze is a subproject (top-level
    # CMakeLists.txt), which is the one case where doctest is not a dependency
    # the consumer asked for and the test binaries are not theirs to build.
    if(ARG_TEST_SOURCES AND BRONZE_BUILD_TESTS)
        add_executable(bronze_${name}_tests ${ARG_TEST_SOURCES})
        target_link_libraries(bronze_${name}_tests PRIVATE bronze::${name} doctest::doctest)
        foreach(dep IN LISTS ARG_TEST_DEPS)
            target_link_libraries(bronze_${name}_tests PRIVATE bronze::${dep})
        endforeach()
        if(MSVC)
            target_compile_options(bronze_${name}_tests PRIVATE /W4 /WX /permissive-)
            target_compile_definitions(bronze_${name}_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            target_compile_options(bronze_${name}_tests PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(bronze_${name}_tests PRIVATE -Wno-restrict)
            endif()
        endif()
        add_test(NAME ${name} COMMAND bronze_${name}_tests)
        set_tests_properties(${name} PROPERTIES LABELS ${name})
    endif()
endfunction()
