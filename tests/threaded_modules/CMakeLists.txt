# Two compiled modules, two threads, two runtimes, one process — the
# generated-code half of the per-thread isolation story, and the executable
# form of embed.h's threading contract. Built exactly the two_module way: the
# objects are emitted at BUILD time by the bronze that was just compiled, so
# the ctest run is a link-free execute-and-compare.

set(_thr_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_thr_bin ${CMAKE_CURRENT_BINARY_DIR})

function(bronze_threaded_module_object js entry out)
    add_custom_command(
        OUTPUT ${out}
        COMMAND $<TARGET_FILE:bronze> build ${js} --emit-obj --entry-symbol=${entry} -o ${out}
        DEPENDS bronze ${js}
        COMMENT "bronze --emit-obj --entry-symbol=${entry}"
        VERBATIM)
endfunction()

bronze_threaded_module_object(${_thr_src}/mod_a.js bronze_thread_a ${_thr_bin}/mod_a.obj)
bronze_threaded_module_object(${_thr_src}/mod_b.js bronze_thread_b ${_thr_bin}/mod_b.obj)

set_source_files_properties(${_thr_bin}/mod_a.obj ${_thr_bin}/mod_b.obj
    PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(bronze_threaded_modules_host
    host.cpp
    ${_thr_bin}/mod_a.obj
    ${_thr_bin}/mod_b.obj)
target_link_libraries(bronze_threaded_modules_host PRIVATE bronze::embed)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Same non-PIC objects, same -no-pie necessity as two_module.
    target_link_options(bronze_threaded_modules_host PRIVATE -no-pie)
endif()
find_package(Threads REQUIRED)
target_link_libraries(bronze_threaded_modules_host PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(bronze_threaded_modules_host PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(bronze_threaded_modules_host PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME threaded-modules COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_threaded_modules_host>
    -DEXPECTED=${_thr_src}/expected.txt
    -DACTUAL=${_thr_bin}/actual.txt
    -P ${_thr_src}/run.cmake)
set_tests_properties(threaded-modules PROPERTIES LABELS threaded_modules)

# The same binary under per-allocation collection on BOTH threads at once —
# every hammer allocation on each thread crosses a collection of that thread's
# heap while the other thread is mid-allocation. This is the run that decides
# whether per-thread state is actually per-thread.
add_test(NAME threaded-modules-gc-stress COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_threaded_modules_host>
    -DEXPECTED=${_thr_src}/expected.txt
    -DACTUAL=${_thr_bin}/actual_gc.txt
    -P ${_thr_src}/run.cmake)
set_tests_properties(threaded-modules-gc-stress PROPERTIES
    LABELS threaded_modules
    ENVIRONMENT "BRONZE_GC_STRESS=1")
