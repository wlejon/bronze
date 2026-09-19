# The program host: what `bronze build` copies under the output's name, beside
# the module it wrote (host_main.cpp has the shape). It exists only where the
# shared runtime does — it IS a loading host, and the driver refuses to build
# an executable without both — so a tree that opted out of the runtime gets
# no host and no dangling reference to it.
if(NOT TARGET bronze_runtime_shared)
    return()
endif()

add_executable(bronze_host host_main.cpp)
# bronze::runtime_shared and NOTHING else from bronze: a second copy of the
# runtime's state in this process is the two-heaps failure the shared runtime
# exists to rule out (cmake/bronze_shared_runtime.cmake).
target_link_libraries(bronze_host PRIVATE bronze::runtime_shared ${CMAKE_DL_LIBS})
# Beside the runtime, which is where the driver looks for both of them, and
# where the loader of a copied host finds the runtime it links — the copy and
# its runtime always travel together (src/cli/link.cpp stages both).
set_target_properties(bronze_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${BRONZE_SHARED_RUNTIME_DIR}")

if(MSVC)
    target_compile_options(bronze_host PRIVATE /W4 /WX /permissive-)
    target_compile_definitions(bronze_host PRIVATE _CRT_SECURE_NO_WARNINGS)
    # 8 MB main-thread stack, like bro's and the bronze CLI's: the 1 MB
    # default caps JS recursion near 7,000 frames before the runtime's
    # stack-limit check turns it into a RangeError.
    target_link_options(bronze_host PRIVATE /STACK:8388608)
else()
    target_compile_options(bronze_host PRIVATE -Wall -Wextra -Werror)
    # The runtime is beside the host wherever the host is copied to, so the
    # host's own directory is its run-time search path — in the build tree
    # and in a program directory alike.
    if(APPLE)
        set_target_properties(bronze_host PROPERTIES
            BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
    else()
        set_target_properties(bronze_host PROPERTIES
            BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()
endif()

# The C++ runtime the host and the shared runtime load — msvcp140,
# msvcp140_2, vcruntime140, vcruntime140_1 — copied to `redist/` beside the
# runtime, from where `bronze build` stages them beside every program
# (src/cli/link.cpp, stageRedist), so a program runs on a machine that never
# installed the VC redistributable. The `api-ms-win-crt-*` names it also
# imports are forwarders every supported Windows carries and need nothing.
# InstallRequiredSystemLibraries is CMake's own knowledge of where the
# toolchain keeps them; with the SKIP it only lists them. A host on the static
# CRT leaves no folder, and the driver stages nothing. The list is the release
# CRT whatever the configuration: a Debug host links the debug CRT, which is
# not redistributable, and a Debug program is a developer's own anyway.
if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded(Debug)?$")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    set(_bronze_redist)
    foreach(_lib IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        get_filename_component(_name "${_lib}" NAME)
        string(TOLOWER "${_name}" _name)
        if(_name MATCHES "^(msvcp140|msvcp140_2|vcruntime140|vcruntime140_1)\\.dll$")
            list(APPEND _bronze_redist "${_lib}")
        endif()
    endforeach()
    if(_bronze_redist)
        add_custom_command(TARGET bronze_host POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${BRONZE_SHARED_RUNTIME_DIR}/redist"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_bronze_redist}
                    "${BRONZE_SHARED_RUNTIME_DIR}/redist"
            COMMENT "Staging the C++ runtime beside the shared runtime (redist/)"
            VERBATIM)
    else()
        message(STATUS "bronze: no redistributable C++ runtime found for this toolchain; "
            "built programs will need the VC redistributable installed")
    endif()
endif()

# Anything that builds programs — the CLI, the oracle and CLI test binaries —
# needs the host on disk before it runs, and it links the driver library.
if(TARGET bronze_cli)
    add_dependencies(bronze_cli bronze_host)
endif()
