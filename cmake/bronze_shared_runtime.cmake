# bronze's runtime as a DLL/.so/.dylib, for a host that LOADS compiled modules
# instead of linking them.
#
# The static path is the one a standalone `bronze build` takes and it is not
# touched here: `bronze_runtime`, `bronze_rt` and every module that links them
# are exactly what they were. This target is an ADDITION — a second image built
# from the same sources — and the reason it exists is the one thing a static
# runtime cannot do: a host process that dlopen's two compiled modules must
# have ONE runtime in it. Two static copies means two heaps, two exception
# cells and two key registries, and a value handed from one module to the other
# is an address in a semispace the other collector is free to reuse.
#
# Which is why the sources are COMPILED AGAIN rather than pulled out of the
# static archives. Reusing the archives would need whole-archive tricks on
# Windows and -fPIC archives on ELF, and the second of those changes the code
# generated for the static build to serve the shared one. A separate
# compilation costs build time and nothing else.
#
# What is in the image: the runtime, the two libraries it owns (json, regex),
# the ABI translation unit, and the embed API. embed belongs here because the
# host↔runtime boundary is a C++ one — embed calls the runtime through C++
# types (ObjectHeader, Rooted, Heap&) that no export list can carry across a
# library boundary. Inside the DLL those calls are ordinary internal calls, and
# what the host sees is embed's own annotated surface.
#
# src/rt is NOT here: it defines `main`.

option(BRONZE_BUILD_SHARED_RUNTIME
       "Build bronze's runtime as a shared library (the loadable-module host path)"
       ${BRONZE_IS_TOP_LEVEL})

if(NOT BRONZE_BUILD_SHARED_RUNTIME)
    return()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/bronze_abi_exports.cmake)

# Everything the shared runtime is built from, read off the static targets
# rather than relisted. A source added to src/runtime/CMakeLists.txt is in this
# image the moment it is in that one — the same "one list, two consumers"
# discipline the ABI registry follows.
#
# One source is left out, and it is left out for the same reason it exists:
# src/embed/embed_run.cpp is the translation unit that NAMES `bronze_main` and
# `bronze_object_abi_fingerprint`, quarantined there so that only a host with a
# statically linked module ever pulls it in. A shared runtime has no linked
# module by definition — its modules arrive at run time, under whatever names
# `--entry-symbol` gave them — so the two symbols would be unresolved externals
# in a library that can never define them. `runMain` and `runProgram` are
# therefore absent from the shared runtime; `runEntry` (embed_module.cpp) is
# the same sequence with the entry passed in, which is what a loading host has.
set(_bronze_shared_sources "")
set(_bronze_shared_excluded embed_run.cpp)
foreach(_mod IN ITEMS abi json regex runtime embed)
    get_target_property(_mod_dir bronze_${_mod} SOURCE_DIR)
    get_target_property(_mod_srcs bronze_${_mod} SOURCES)
    foreach(_src IN LISTS _mod_srcs)
        if(NOT _src IN_LIST _bronze_shared_excluded)
            list(APPEND _bronze_shared_sources "${_mod_dir}/${_src}")
        endif()
    endforeach()
endforeach()

set(BRONZE_SHARED_RUNTIME_DIR "${CMAKE_BINARY_DIR}/shared" CACHE INTERNAL
    "Where the shared runtime, its import library and anything that loads it live")

bronze_abi_export_files("${CMAKE_SOURCE_DIR}/src/abi/bronze_abi.h"
                        "${CMAKE_CURRENT_BINARY_DIR}"
                        _bronze_abi_def _bronze_abi_ver _bronze_abi_exp)

add_library(bronze_runtime_shared SHARED ${_bronze_shared_sources})
add_library(bronze::runtime_shared ALIAS bronze_runtime_shared)

target_include_directories(bronze_runtime_shared PUBLIC ${CMAKE_SOURCE_DIR}/src)

# BRONZE_ABI_FINGERPRINT, copied off the target that computes it rather than
# recomputed: two hashes of one header are two chances to disagree.
get_target_property(_bronze_abi_defs bronze_abi INTERFACE_COMPILE_DEFINITIONS)
target_compile_definitions(bronze_runtime_shared PUBLIC ${_bronze_abi_defs})

# The export macro's two states (src/embed/embed.h has the contract): the
# library is BUILDING the boundary, anything that links this target is USING
# it. Neither is defined for the static libraries, where the macro expands to
# nothing and the static path stays byte-identical.
target_compile_definitions(bronze_runtime_shared PRIVATE BRONZE_EMBED_SHARED_BUILD)
target_compile_definitions(bronze_runtime_shared INTERFACE BRONZE_EMBED_SHARED)

set_target_properties(bronze_runtime_shared PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    # CMake's "export everything" convenience is exactly what this target must
    # not do: the surface is the generated list plus the annotated C++ API.
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    RUNTIME_OUTPUT_DIRECTORY "${BRONZE_SHARED_RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${BRONZE_SHARED_RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${BRONZE_SHARED_RUNTIME_DIR}")

if(MSVC)
    target_compile_options(bronze_runtime_shared PRIVATE /W4 /WX /permissive-)
    # A .def source is how CMake spells /DEF: for the MSVC-family linkers.
    target_sources(bronze_runtime_shared PRIVATE ${_bronze_abi_def})
    set_source_files_properties(${_bronze_abi_def} PROPERTIES GENERATED TRUE)
else()
    target_compile_options(bronze_runtime_shared PRIVATE
        -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(bronze_runtime_shared PRIVATE -Wno-restrict)
    endif()
    if(APPLE)
        target_link_options(bronze_runtime_shared PRIVATE
            "LINKER:-exported_symbols_list,${_bronze_abi_exp}")
        set_property(TARGET bronze_runtime_shared APPEND
                     PROPERTY LINK_DEPENDS ${_bronze_abi_exp})
    else()
        target_link_options(bronze_runtime_shared PRIVATE
            "LINKER:--version-script,${_bronze_abi_ver}")
        set_property(TARGET bronze_runtime_shared APPEND
                     PROPERTY LINK_DEPENDS ${_bronze_abi_ver})
    endif()
endif()
