# N compiled modules, one runtime, one process.
#
# The only test in the tree that compiles JavaScript to OBJECTS and links them
# into a host of its own — which is the whole point, because every piece of
# bookkeeping this exercises (key interning, the module-local caches, the
# module-environment cell, the per-module entry and ABI stamp) is correct by
# construction as long as there is exactly one compiled module in the image.
#
# The two objects are built at BUILD time by the bronze that was just compiled,
# so the ctest run itself is a link-free execute-and-compare and stays in the
# `embed` label's seconds-long budget.

set(_two_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_two_bin ${CMAKE_CURRENT_BINARY_DIR})

function(bronze_two_module_object js entry out)
    add_custom_command(
        OUTPUT ${out}
        COMMAND $<TARGET_FILE:bronze> build ${js} --emit-obj --entry-symbol=${entry} -o ${out}
        DEPENDS bronze ${js}
        COMMENT "bronze --emit-obj --entry-symbol=${entry}"
        VERBATIM)
endfunction()

bronze_two_module_object(${_two_src}/mod_a.js bronze_module_a ${_two_bin}/mod_a.obj)
bronze_two_module_object(${_two_src}/mod_b.js bronze_module_b ${_two_bin}/mod_b.obj)

# Handed to the linker as objects rather than compiled from source. If the two
# ever collide on a symbol, THIS is where it surfaces — a duplicate-symbol error
# at link, which is the check that everything but the three symbols named after
# the entry (the entry, the ABI stamp, the host-globals manifest) really does
# have internal linkage.
set_source_files_properties(${_two_bin}/mod_a.obj ${_two_bin}/mod_b.obj
    PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(bronze_two_module_host
    host.cpp
    ${_two_bin}/mod_a.obj
    ${_two_bin}/mod_b.obj)
target_link_libraries(bronze_two_module_host PRIVATE bronze::embed)
# bronze emits non-PIC objects (its own driver links executables -no-pie on
# Linux, link.cpp); a distro-default PIE link of them dies in GNU ld with
# "failed to set dynamic section sizes: bad value".
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(bronze_two_module_host PRIVATE -no-pie)
endif()
if(MSVC)
    target_compile_options(bronze_two_module_host PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(bronze_two_module_host PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME two-module COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_two_module_host>
    -DEXPECTED=${_two_src}/expected.txt
    -DACTUAL=${_two_bin}/actual.txt
    -P ${_two_src}/run.cmake)
set_tests_properties(two-module PROPERTIES LABELS embed)

# The same binary under per-allocation collection. The module-local cache,
# fn-slot and module-environment spans are registered GC roots, and this is the
# run that decides whether they are traced: every read of a cached global and
# every mention of a module's own function declaration crosses a collection.
add_test(NAME two-module-gc-stress COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_two_module_host>
    -DEXPECTED=${_two_src}/expected.txt
    -DACTUAL=${_two_bin}/actual_gc.txt
    -P ${_two_src}/run.cmake)
set_tests_properties(two-module-gc-stress PROPERTIES
    LABELS embed
    ENVIRONMENT "BRONZE_GC_STRESS=1")
