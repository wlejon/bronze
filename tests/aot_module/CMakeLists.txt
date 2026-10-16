# Scripts compiled ahead of time through the embedded evaluator (eval.h,
# "Ahead-of-time modules"): build, load, run, and the record's refusals.
#
# Unlike tests/shared_load, nothing is built at build time: writing the module
# is the thing under test, and the harness does it in process, with no linker,
# exactly as a host installing an app would.

add_executable(bronze_aot_module_host harness.cpp)
# bronze::runtime_shared and NOTHING else from bronze (tests/shared_load says
# why: a static copy of the runtime beside the shared one is two heaps).
target_link_libraries(bronze_aot_module_host PRIVATE bronze::runtime_shared ${CMAKE_DL_LIBS})
# Beside the runtime library, so a loaded module's import resolves to the
# runtime this process already mapped (Windows resolves out of the executable's
# directory).
set_target_properties(bronze_aot_module_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BRONZE_SHARED_RUNTIME_DIR})
if(MSVC)
    target_compile_options(bronze_aot_module_host PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(bronze_aot_module_host PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME aot-module COMMAND bronze_aot_module_host)
set_tests_properties(aot-module PROPERTIES LABELS shared)

# The same run under per-allocation collection: the loaded module's frames are
# walked through the stack maps its entry registered, and everything the
# harness holds across a call lives in a Persistent.
add_test(NAME aot-module-gc-stress COMMAND bronze_aot_module_host)
set_tests_properties(aot-module-gc-stress PROPERTIES
    LABELS shared
    ENVIRONMENT "BRONZE_GC_STRESS=1")
