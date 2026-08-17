# A compiled module LOADED at run time, against a shared runtime.
#
# The only test in the tree where bronze's runtime is a library rather than an
# archive, and the only one where a module is opened rather than linked — which
# together are the whole of what an external host (bro) needs and what nothing
# else here exercised. tests/two_module is the static half of the same story:
# N modules, one runtime, resolved at ITS link step.
#
# Three artefacts, all built at BUILD time so the ctest run is a launch and a
# byte comparison:
#
#   the module   `bronze build --emit-shared`, by the bronze just compiled
#   the fake     a hand-written library with a deliberately wrong ABI stamp
#   the harness  a host linked ONLY against the shared runtime
#
# That last "only" is load-bearing and is the reason the harness is its own
# executable rather than a case in the embed doctests: those link the static
# bronze::embed, and a process holding both copies would have two heaps, which
# is exactly the failure this test exists to rule out.

set(_sl_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_sl_bin ${CMAKE_CURRENT_BINARY_DIR})

# BRONZE_SHARED_RT_LIB rather than letting the driver search: the test knows
# exactly which runtime this module must bind to, and a search that found a
# stale one somewhere else would be a silently different test.
set(_sl_module ${_sl_bin}/bronze_shared_load_module${CMAKE_SHARED_LIBRARY_SUFFIX})
add_custom_command(
    OUTPUT ${_sl_module}
    COMMAND ${CMAKE_COMMAND} -E env
            BRONZE_SHARED_RT_LIB=$<TARGET_LINKER_FILE:bronze_runtime_shared>
            $<TARGET_FILE:bronze> build ${_sl_src}/mod.js
            --emit-shared
            --entry-symbol=bronze_shared_demo
            --host-globals ${_sl_src}/host_globals.txt
            -o ${_sl_module}
    DEPENDS bronze bronze_runtime_shared ${_sl_src}/mod.js ${_sl_src}/host_globals.txt
    COMMENT "bronze --emit-shared (shared-load module)"
    VERBATIM)
add_custom_target(bronze_shared_load_module DEPENDS ${_sl_module})

add_library(bronze_shared_load_fake SHARED fake_module.cpp)
# The runtime's fingerprint, so the fake can be wrong about it BY CONSTRUCTION
# (fake_module.cpp says why a literal would not do). The definition only; the
# fake links no bronze library and must not.
get_target_property(_sl_abi_defs bronze_abi INTERFACE_COMPILE_DEFINITIONS)
target_compile_definitions(bronze_shared_load_fake PRIVATE ${_sl_abi_defs})
set_target_properties(bronze_shared_load_fake PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${_sl_bin}
    LIBRARY_OUTPUT_DIRECTORY ${_sl_bin}
    ARCHIVE_OUTPUT_DIRECTORY ${_sl_bin})

add_executable(bronze_shared_load_host harness.cpp)
# bronze::runtime_shared and NOTHING else from bronze. Adding bronze::embed or
# bronze::runtime here would link a second copy of the runtime's state into
# this process and the test would start passing for the wrong reason.
target_link_libraries(bronze_shared_load_host PRIVATE bronze::runtime_shared ${CMAKE_DL_LIBS})
add_dependencies(bronze_shared_load_host bronze_shared_load_module bronze_shared_load_fake)
# Beside the runtime library, so that on Windows — where a loaded module's
# imports are resolved out of the directory of the EXECUTABLE — the module the
# harness opens finds the same runtime the harness itself bound to.
set_target_properties(bronze_shared_load_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BRONZE_SHARED_RUNTIME_DIR})
if(MSVC)
    target_compile_options(bronze_shared_load_host PRIVATE /W4 /WX /permissive-)
    target_compile_options(bronze_shared_load_fake PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(bronze_shared_load_host PRIVATE -Wall -Wextra -Werror)
    target_compile_options(bronze_shared_load_fake PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME shared-load COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_shared_load_host>
    -DMODULE=${_sl_module}
    -DFAKE=$<TARGET_FILE:bronze_shared_load_fake>
    -DEXPECTED=${_sl_src}/expected.txt
    -DACTUAL=${_sl_bin}/actual.txt
    -P ${_sl_src}/run.cmake)
set_tests_properties(shared-load PROPERTIES LABELS shared)

# The same binary under per-allocation collection. Everything the host holds
# across a call into the module lives in a Persistent and everything the module
# holds is its own registered root span; this is the run that decides whether
# that is true rather than merely intended, now with the module's data in a
# separately loaded image.
# The same module, linked at TEST time with the developer environment scrubbed,
# then run. run_no_vcvars.cmake says why an ordinary build-time link cannot
# stand in for this: ninja runs under dev.cmd and every link in this tree has
# had a %LIB% to lean on, which is exactly the assumption that broke
# `--emit-shared` for anyone invoking bronze from their own shell.
#
# Registered only when a linker survives the scrub, because the scrub removes
# Visual Studio's bundled lld-link along with the rest of the VS installation
# (scrub_path.cmake says why that one has to go). On a machine whose only
# lld-link is the bundled one there is no plain-shell linker to test with, and a
# test that reported failure there would be reporting on the machine rather than
# on bronze. It is announced at configure time rather than skipped at run time
# so that its absence is visible.
include(${_sl_src}/scrub_path.cmake)
bronze_scrub_dev_path(_sl_bare_dirs)
find_program(BRONZE_SHARED_LOAD_BARE_LINKER
    NAMES lld-link
    PATHS ${_sl_bare_dirs}
    NO_DEFAULT_PATH
    DOC "A linker reachable without the MSVC developer environment (shared-load-no-vcvars)")
if(NOT WIN32)
    message(STATUS "shared-load-no-vcvars: not registered (the developer environment "
                   "it scrubs for is a Windows notion)")
elseif(NOT BRONZE_SHARED_LOAD_BARE_LINKER)
    message(STATUS "shared-load-no-vcvars: not registered (no lld-link outside the "
                   "Visual Studio installation on this machine)")
else()
    add_test(NAME shared-load-no-vcvars COMMAND ${CMAKE_COMMAND}
        -DBRONZE=$<TARGET_FILE:bronze>
        -DMODJS=${_sl_src}/mod.js
        -DMANIFEST=${_sl_src}/host_globals.txt
        -DRTLIB=$<TARGET_LINKER_FILE:bronze_runtime_shared>
        -DMODULE=${_sl_bin}/bronze_shared_load_module_bare${CMAKE_SHARED_LIBRARY_SUFFIX}
        -DEXE=$<TARGET_FILE:bronze_shared_load_host>
        -DFAKE=$<TARGET_FILE:bronze_shared_load_fake>
        -DEXPECTED=${_sl_src}/expected.txt
        -DACTUAL=${_sl_bin}/actual_bare.txt
        -P ${_sl_src}/run_no_vcvars.cmake)
    set_tests_properties(shared-load-no-vcvars PROPERTIES LABELS shared)
endif()

add_test(NAME shared-load-gc-stress COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_shared_load_host>
    -DMODULE=${_sl_module}
    -DFAKE=$<TARGET_FILE:bronze_shared_load_fake>
    -DEXPECTED=${_sl_src}/expected.txt
    -DACTUAL=${_sl_bin}/actual_gc.txt
    -P ${_sl_src}/run.cmake)
set_tests_properties(shared-load-gc-stress PROPERTIES
    LABELS shared
    ENVIRONMENT "BRONZE_GC_STRESS=1")
