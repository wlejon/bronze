if(NOT TARGET brass)
    if(DEFINED ENV{BRASS_ROOT} AND NOT "$ENV{BRASS_ROOT}" STREQUAL "")
        set(BRASS_ROOT "$ENV{BRASS_ROOT}")
    elseif(NOT DEFINED BRASS_ROOT OR BRASS_ROOT STREQUAL "")
        foreach(_candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../brass"
            "${CMAKE_SOURCE_DIR}/../brass"
            "${CMAKE_SOURCE_DIR}/third_party/brass"
        )
            if(EXISTS "${_candidate}/include/brass/brass.hpp")
                get_filename_component(BRASS_ROOT "${_candidate}" ABSOLUTE)
                break()
            endif()
        endforeach()
    endif()
    if(EXISTS "${BRASS_ROOT}/CMakeLists.txt")
        file(TO_CMAKE_PATH "${BRASS_ROOT}" BRASS_ROOT)
        set(BRASS_ROOT "${BRASS_ROOT}" CACHE PATH "Path to Brass root directory" FORCE)
        add_subdirectory("${BRASS_ROOT}" "${CMAKE_BINARY_DIR}/brass-build" EXCLUDE_FROM_ALL)
    endif()
endif()
# brass is always compiled in this build tree, never linked prebuilt: the
# backend and the runtime cross a C++ boundary that needs one compiler and one
# CRT on both sides, which a library built elsewhere cannot promise.
if(NOT TARGET brass)
    message(FATAL_ERROR
        "brass not found (BRASS_ROOT='${BRASS_ROOT}'). Check it out beside bronze "
        "(../brass) or pass -DBRASS_ROOT=<brass source tree>.")
endif()

add_library(bronze_codegen_brass STATIC
    brass_backend.cpp
    brass_backend_debug.cpp
    brass_backend_sections.cpp
    brass_coroutine_bridge.cpp
    brass_jit.cpp
    brass_symbol_registration.cpp
    brass_tiered_engine.cpp
    il_to_brass_ast.cpp
    # il2mir: the bronze IL → brass MIR translator.
    il2mir/il_alloc_lowering.cpp
    il2mir/il_lowering.cpp
    il2mir/il_lowering_calls.cpp
    il2mir/il_lowering_coro.cpp
    il2mir/il_lowering_inst.cpp
    il2mir/il_lowering_ops.cpp
    il2mir/il_lowering_wrapper.cpp
    il2mir/il_module_symbols.cpp
    il2mir/il_pipeline.cpp
    il2mir/il_property_lowering.cpp
    il2mir/il_translator.cpp
)
add_library(bronze::codegen_brass ALIAS bronze_codegen_brass)

target_include_directories(bronze_codegen_brass PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
if(DEFINED BRASS_ROOT AND EXISTS "${BRASS_ROOT}/include")
    target_include_directories(bronze_codegen_brass PUBLIC "${BRASS_ROOT}/include")
endif()

find_package(Threads REQUIRED)

set(BRASS_LIB brass)
set(PLATFORM_LIBS)
if(NOT MSVC)
    set(PLATFORM_LIBS ${CMAKE_DL_LIBS} m)
endif()

# bronze::runtime is deliberately NOT linked here, although the tiered engine
# and the coroutine bridge include runtime headers and call into it. This
# library reaches a host through bronze::cli, and a host that loads compiled
# modules links bronze::runtime_shared instead: naming the static runtime here
# would put a second runtime (second heap, key registry, shadow stack) into that
# process — bro aborts at boot with "Rooted<> with no ShadowStackFrame open".
# Whoever links this library supplies exactly one runtime: bronze::eval and the
# tests link the static one, runtime_shared compiles its own.
target_include_directories(bronze_codegen_brass PUBLIC
    $<TARGET_PROPERTY:bronze_runtime,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(bronze_codegen_brass PUBLIC
    bronze::abi
    bronze::codegen
    bronze::il
    bronze::support
    ${BRASS_LIB}
    Threads::Threads
    ${PLATFORM_LIBS}
)

if(MSVC)
    target_compile_options(bronze_codegen_brass PRIVATE /W4 /WX /permissive- /wd4324)
    target_compile_definitions(bronze_codegen_brass PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(bronze_codegen_brass PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
    # GCC 12's -Wrestrict false positive inside std::string's char_traits,
    # as for every bronze_module (cmake/bronze_module.cmake).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(bronze_codegen_brass PRIVATE -Wno-restrict)
    endif()
endif()

if(BRONZE_BUILD_TESTS)
    add_executable(bronze_codegen_brass_tests
        ../../tests/codegen_brass/codegen_brass_test.cpp
        ../../tests/codegen_brass/tiered_engine_test.cpp
        ../../tests/codegen_brass/optimization_test.cpp
        ../../tests/codegen_brass/debug_test.cpp
        ../../tests/codegen_brass/gc_write_barrier_test.cpp
        ../../tests/codegen_brass/coroutine_test.cpp
    )
    target_link_libraries(bronze_codegen_brass_tests PRIVATE
        bronze::codegen_brass
        bronze::eval
        bronze::embed
        bronze::cli
        bronze::runtime
        doctest::doctest
    )
    if(MSVC)
        target_compile_options(bronze_codegen_brass_tests PRIVATE /W4 /WX /permissive-)
        target_compile_definitions(bronze_codegen_brass_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_options(bronze_codegen_brass_tests PRIVATE /FORCE:MULTIPLE)
    else()
        target_compile_options(bronze_codegen_brass_tests PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(bronze_codegen_brass_tests PRIVATE -Wno-restrict)
        endif()
    endif()
    add_test(NAME codegen_brass COMMAND bronze_codegen_brass_tests)
    set_tests_properties(codegen_brass PROPERTIES LABELS codegen_brass)
endif()
