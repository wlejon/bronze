# A compiled module LINKED into a host that uses the shared runtime and also
# links brass statically (host.cpp says why that is the shape that broke):
# every generator and async function frame the module starts must come from
# the runtime's coroutine runtime, not the host's copy.

set(_sb_src ${CMAKE_CURRENT_SOURCE_DIR})
set(_sb_bin ${CMAKE_CURRENT_BINARY_DIR})

add_custom_command(
    OUTPUT ${_sb_bin}/mod.obj
    COMMAND $<TARGET_FILE:bronze> build ${_sb_src}/mod.js --emit-obj
            --entry-symbol=bronze_static_brass_mod -o ${_sb_bin}/mod.obj
    DEPENDS bronze ${_sb_src}/mod.js
    COMMENT "bronze --emit-obj (static-brass host module)"
    VERBATIM)
set_source_files_properties(${_sb_bin}/mod.obj PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(bronze_static_brass_host host.cpp ${_sb_bin}/mod.obj)
# brass itself, then the shared runtime: the archive comes first on the link
# line, as a compiler linked into the host puts it, so a canonical name it
# defined would win. NOT brass_bind_eh_symbols, which would bind the canonical
# names to this image's brass and test nothing.
target_link_libraries(bronze_static_brass_host PRIVATE brass bronze::runtime_shared ${CMAKE_DL_LIBS})
# Beside the runtime library, so Windows finds it at launch.
set_target_properties(bronze_static_brass_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BRONZE_SHARED_RUNTIME_DIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # bronze emits non-PIC objects (tests/two_module says more).
    target_link_options(bronze_static_brass_host PRIVATE -no-pie)
endif()
if(MSVC)
    target_compile_options(bronze_static_brass_host PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(bronze_static_brass_host PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME static-brass-host COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_static_brass_host>
    -DEXPECTED=${_sb_src}/expected.txt
    -DACTUAL=${_sb_bin}/actual.txt
    -P ${_sb_src}/run.cmake)
set_tests_properties(static-brass-host PROPERTIES LABELS embed)

add_test(NAME static-brass-host-gc-stress COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:bronze_static_brass_host>
    -DEXPECTED=${_sb_src}/expected.txt
    -DACTUAL=${_sb_bin}/actual_gc.txt
    -P ${_sb_src}/run.cmake)
set_tests_properties(static-brass-host-gc-stress PROPERTIES
    LABELS embed
    ENVIRONMENT "BRONZE_GC_STRESS=1")
