# Hot swap: one module, two versions, one process, one shared runtime.
#
# The unload half of the loadable-module story tests/shared_load establishes:
# that test proves a host can LOAD a module against the shared runtime; this
# one proves it can RETIRE one and load its replacement. harness.cpp states
# what each step asserts — most importantly that the two versions' module-scope
# payloads are sized so both cannot fit one semispace, which turns "unload
# really dropped the roots" from an intention into the condition for the
# process surviving at all.
#
# Same artefact discipline as shared_load: everything is built at BUILD time,
# so the ctest run is a launch and a byte comparison.

set(_hs_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_hs_bin ${CMAKE_CURRENT_BINARY_DIR})

# Both versions compile with the SAME entry symbol: they are two builds of one
# module, distinguishable only by which file the host opens — which is exactly
# the shape a hot-reloading host produces.
foreach(_hs_ver v1 v2)
    set(_hs_${_hs_ver}_module ${_hs_bin}/bronze_hot_${_hs_ver}${CMAKE_SHARED_LIBRARY_SUFFIX})
    add_custom_command(
        OUTPUT ${_hs_${_hs_ver}_module}
        COMMAND ${CMAKE_COMMAND} -E env
                BRONZE_SHARED_RT_LIB=$<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE:bronze> build ${_hs_src}/${_hs_ver}.js
                --emit-shared
                --entry-symbol=bronze_hot_module
                -o ${_hs_${_hs_ver}_module}
        DEPENDS bronze bronze_runtime_shared ${_hs_src}/${_hs_ver}.js
        COMMENT "bronze --emit-shared (hot-swap ${_hs_ver})"
        VERBATIM)
endforeach()
add_custom_target(bronze_hot_swap_modules
    DEPENDS ${_hs_v1_module} ${_hs_v2_module})

add_executable(bronze_hot_swap_host harness.cpp)
# bronze::runtime_shared and NOTHING else from bronze, for the reason
# tests/shared_load/CMakeLists.txt states: a second (static) copy of the
# runtime's state in this process would mean two heaps, and the test would
# start passing for the wrong reason.
target_link_libraries(bronze_hot_swap_host PRIVATE bronze::runtime_shared ${CMAKE_DL_LIBS})
add_dependencies(bronze_hot_swap_host bronze_hot_swap_modules)
# Beside the runtime library, so a loaded module's imports resolve to the same
# runtime the harness bound to (Windows resolves out of the executable's
# directory).
set_target_properties(bronze_hot_swap_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BRONZE_SHARED_RUNTIME_DIR})
if(MSVC)
    target_compile_options(bronze_hot_swap_host PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(bronze_hot_swap_host PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME hot-swap COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_hot_swap_host>
    -DV1=${_hs_v1_module}
    -DV2=${_hs_v2_module}
    -DEXPECTED=${_hs_src}/expected.txt
    -DACTUAL=${_hs_bin}/actual.txt
    -P ${_hs_src}/run.cmake)
set_tests_properties(hot-swap PROPERTIES LABELS hot_swap)

# The same binary under per-allocation collection: the swap window — spans
# dropped, globalThis overwritten mid-entry, 24MB dying while 24MB is born —
# is exactly the kind of window a stale root survives only by luck.
add_test(NAME hot-swap-gc-stress COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_hot_swap_host>
    -DV1=${_hs_v1_module}
    -DV2=${_hs_v2_module}
    -DEXPECTED=${_hs_src}/expected.txt
    -DACTUAL=${_hs_bin}/actual_gc.txt
    -P ${_hs_src}/run.cmake)
set_tests_properties(hot-swap-gc-stress PROPERTIES
    LABELS hot_swap
    ENVIRONMENT "BRONZE_GC_STRESS=1")
