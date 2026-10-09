# brass, through cmake/bro_deps.cmake: an outer project's brass target, else
# the ../brass working tree beside the top-level project, else the commit
# pinned in the top-level CMakeLists.txt, fetched. A BRASS_ROOT environment
# variable or -DBRASS_ROOT names another tree to build it from. BRASS_ROOT in
# the cache then reports the tree chosen (the runtimes read brass's headers
# from it). brass is always compiled in this build tree, never linked prebuilt:
# the backend and the runtime cross a C++ boundary that needs one compiler and
# one CRT on both sides, which a library built elsewhere cannot promise.
if(NOT TARGET brass)
    if(DEFINED ENV{BRASS_ROOT} AND NOT "$ENV{BRASS_ROOT}" STREQUAL "")
        set(FETCHCONTENT_SOURCE_DIR_BRASS "$ENV{BRASS_ROOT}")
    elseif(BRASS_ROOT AND NOT BRASS_ROOT STREQUAL _BRONZE_BRASS_ROOT_CHOSEN)
        set(FETCHCONTENT_SOURCE_DIR_BRASS "${BRASS_ROOT}")
    endif()
    bro_dependency(brass)
    set(BRASS_ROOT "${brass_SOURCE_DIR}" CACHE PATH "brass source tree" FORCE)
    set(_BRONZE_BRASS_ROOT_CHOSEN "${brass_SOURCE_DIR}" CACHE INTERNAL "")
endif()

add_library(bronze_codegen_brass STATIC
    brass_backend.cpp
    brass_backend_debug.cpp
    brass_backend_sections.cpp
    brass_jit.cpp
    brass_symbol_registration.cpp
    brass_tiered_engine.cpp
    brass_tiered_image.cpp
    il_to_brass_ast.cpp
    # il2mir: the bronze IL → brass MIR translator.
    il2mir/il_alloc_lowering.cpp
    il2mir/il_elem_lowering.cpp
    il2mir/il_ic_lowering.cpp
    il2mir/il_ic_stub.cpp
    il2mir/il_lowering.cpp
    il2mir/il_lowering_calls.cpp
    il2mir/il_lowering_coro.cpp
    il2mir/il_lowering_eh.cpp
    il2mir/il_lowering_hoist.cpp
    il2mir/il_lowering_inst.cpp
    il2mir/il_lowering_lazy.cpp
    il2mir/il_lowering_ops.cpp
    il2mir/il_lowering_wrapper.cpp
    il2mir/il_module_symbols.cpp
    il2mir/il_pipeline.cpp
    il2mir/il_property_lowering.cpp
    il2mir/il_spec_profile.cpp
    il2mir/il_speculation.cpp
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
# and the symbol registration include runtime headers and call into it. This
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

# brass_tiered_engine.cpp jitVerifiesMir: verify in-process MIR in Debug and in
# bronze's own test builds, not in a host's release build.
if(BRONZE_BUILD_TESTS)
    target_compile_definitions(bronze_codegen_brass PRIVATE BRONZE_VERIFY_MIR_DEFAULT=1)
else()
    target_compile_definitions(bronze_codegen_brass PRIVATE
        BRONZE_VERIFY_MIR_DEFAULT=$<IF:$<CONFIG:Debug>,1,0>)
endif()

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
    else()
        target_compile_options(bronze_codegen_brass_tests PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(bronze_codegen_brass_tests PRIVATE -Wno-restrict)
        endif()
    endif()
    add_test(NAME codegen_brass COMMAND bronze_codegen_brass_tests)
    set_tests_properties(codegen_brass PROPERTIES LABELS codegen_brass)
endif()
