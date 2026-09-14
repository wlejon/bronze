# Natives registered by the host and bound at load time — both paths.
#
#   JIT     bronze_native_tests: doctest cases that register natives.h's
#           functions and evaluate programs against them (native_test.cpp).
#   AOT     a manifest printed by the registry (manifest_tool.cpp), a module
#           compiled against it with `bronze build --emit-shared
#           --native-manifest`, and a harness linked ONLY against the shared
#           runtime that opens the module, binds its import table and enters
#           it (harness.cpp). Needs the shared runtime, like tests/shared_load.

set(_nt_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_nt_bin ${CMAKE_CURRENT_BINARY_DIR})

if(MSVC)
    set(_nt_warnings /W4 /WX /permissive-)
else()
    set(_nt_warnings -Wall -Wextra -Werror)
endif()

# ---- JIT ---------------------------------------------------------------------

add_executable(bronze_native_tests native_test.cpp)
target_link_libraries(bronze_native_tests PRIVATE bronze::eval bronze::embed doctest::doctest)
target_compile_options(bronze_native_tests PRIVATE ${_nt_warnings})
if(MSVC)
    target_compile_definitions(bronze_native_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(bronze_native_tests PRIVATE /FORCE:MULTIPLE)
endif()
add_test(NAME native COMMAND bronze_native_tests)
set_tests_properties(native PROPERTIES LABELS native)
# Per-allocation collection: handles, typed-array pointers and str copies all
# live across allocating calls, and this is the run that decides whether the
# ordering the lowering promises (scalars first, pointers last) holds.
add_test(NAME native-gc-stress COMMAND bronze_native_tests)
set_tests_properties(native-gc-stress PROPERTIES
    LABELS native
    ENVIRONMENT "BRONZE_GC_STRESS=1")

# ---- AOT ---------------------------------------------------------------------

if(NOT BRONZE_BUILD_SHARED_RUNTIME)
    message(STATUS "native-shared-load: not registered (BRONZE_BUILD_SHARED_RUNTIME is off)")
    return()
endif()

# The manifest writer links the STATIC embed: it is a build step, its own
# process, and never in the same process as the harness.
add_executable(bronze_native_manifest_tool manifest_tool.cpp)
target_link_libraries(bronze_native_manifest_tool PRIVATE bronze::embed)
target_compile_options(bronze_native_manifest_tool PRIVATE ${_nt_warnings})
if(MSVC)
    target_link_options(bronze_native_manifest_tool PRIVATE /FORCE:MULTIPLE)
endif()

set(_nt_manifest ${_nt_bin}/natives.json)
set(_nt_manifest_missing ${_nt_bin}/natives_missing.json)
add_custom_command(
    OUTPUT ${_nt_manifest}
    COMMAND bronze_native_manifest_tool ${_nt_manifest}
    DEPENDS bronze_native_manifest_tool
    COMMENT "native manifest (registry -> JSON)"
    VERBATIM)
add_custom_command(
    OUTPUT ${_nt_manifest_missing}
    COMMAND bronze_native_manifest_tool ${_nt_manifest_missing} --with-absent
    DEPENDS bronze_native_manifest_tool
    COMMENT "native manifest with one native no harness registers"
    VERBATIM)

set(_nt_module ${_nt_bin}/bronze_native_module${CMAKE_SHARED_LIBRARY_SUFFIX})
add_custom_command(
    OUTPUT ${_nt_module}
    COMMAND ${CMAKE_COMMAND} -E env
            BRONZE_SHARED_RT_LIB=$<TARGET_LINKER_FILE:bronze_runtime_shared>
            $<TARGET_FILE:bronze> build ${_nt_src}/mod.js
            --emit-shared
            --entry-symbol=bronze_native_demo
            --native-manifest ${_nt_manifest}
            --host-globals ${_nt_src}/host_globals.txt
            -o ${_nt_module}
    DEPENDS bronze bronze_runtime_shared ${_nt_src}/mod.js ${_nt_src}/host_globals.txt ${_nt_manifest}
    COMMENT "bronze --emit-shared --native-manifest (native module)"
    VERBATIM)
set(_nt_module_missing ${_nt_bin}/bronze_native_module_missing${CMAKE_SHARED_LIBRARY_SUFFIX})
add_custom_command(
    OUTPUT ${_nt_module_missing}
    COMMAND ${CMAKE_COMMAND} -E env
            BRONZE_SHARED_RT_LIB=$<TARGET_LINKER_FILE:bronze_runtime_shared>
            $<TARGET_FILE:bronze> build ${_nt_src}/mod_missing.js
            --emit-shared
            --entry-symbol=bronze_native_missing
            --native-manifest ${_nt_manifest_missing}
            -o ${_nt_module_missing}
    DEPENDS bronze bronze_runtime_shared ${_nt_src}/mod_missing.js ${_nt_manifest_missing}
    COMMENT "bronze --emit-shared --native-manifest (module naming an absent native)"
    VERBATIM)
add_custom_target(bronze_native_modules DEPENDS ${_nt_module} ${_nt_module_missing})

add_executable(bronze_native_host harness.cpp)
# bronze::runtime_shared and NOTHING else from bronze (tests/shared_load says why).
target_link_libraries(bronze_native_host PRIVATE bronze::runtime_shared ${CMAKE_DL_LIBS})
target_compile_options(bronze_native_host PRIVATE ${_nt_warnings})
add_dependencies(bronze_native_host bronze_native_modules)
set_target_properties(bronze_native_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BRONZE_SHARED_RUNTIME_DIR})

add_test(NAME native-shared-load COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_native_host>
    -DMODULE=${_nt_module}
    -DMISSING=${_nt_module_missing}
    -DEXPECTED=${_nt_src}/expected.txt
    -DACTUAL=${_nt_bin}/actual.txt
    -P ${_nt_src}/run.cmake)
set_tests_properties(native-shared-load PROPERTIES LABELS native)

add_test(NAME native-shared-load-gc-stress COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_native_host>
    -DMODULE=${_nt_module}
    -DMISSING=${_nt_module_missing}
    -DEXPECTED=${_nt_src}/expected.txt
    -DACTUAL=${_nt_bin}/actual_gc.txt
    -P ${_nt_src}/run.cmake)
set_tests_properties(native-shared-load-gc-stress PROPERTIES
    LABELS native
    ENVIRONMENT "BRONZE_GC_STRESS=1")
