cmake_minimum_required(VERSION 3.24)

# Is bronze the build, or a piece of someone else's? bro consumes it with
# add_subdirectory, and the two things a subproject must not do are decide the
# toolchain (the outer project already did) and register tests into the outer
# project's ctest. `PROJECT_IS_TOP_LEVEL` cannot answer this: it is set by
# project(), and the toolchain block below has to run before that call.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(BRONZE_IS_TOP_LEVEL ON)
else()
    set(BRONZE_IS_TOP_LEVEL OFF)
endif()

# vcpkg toolchain. Resolution order (same pattern as bro):
#   1. -DCMAKE_TOOLCHAIN_FILE=... on the command line (respected as-is)
#   2. $VCPKG_ROOT environment variable
#   3. Known machine locations (D:/vcpkg is where bro builds from)
#
# Skipped entirely for a subproject: a toolchain file takes effect at the
# top-level project() call and nowhere else, so setting one here would either
# be ignored or — worse, on a fresh cache — fight the host's own choice.
if(BRONZE_IS_TOP_LEVEL AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(_bronze_vcpkg_root "$ENV{VCPKG_ROOT}")
    else()
        foreach(_candidate "D:/vcpkg" "$ENV{HOME}/vcpkg" "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg")
            if(EXISTS "${_candidate}/scripts/buildsystems/vcpkg.cmake")
                set(_bronze_vcpkg_root "${_candidate}")
                message(STATUS "vcpkg: auto-detected at ${_bronze_vcpkg_root}")
                break()
            endif()
        endforeach()
    endif()
    if(DEFINED _bronze_vcpkg_root)
        set(CMAKE_TOOLCHAIN_FILE "${_bronze_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()


if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "OSX Architectures" FORCE)
endif()

# C is enabled solely for src/abi/abi_check.c, which compiles the
# generated-code ABI header as pure C to keep C++ types out of it.
project(bronze VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/bronze_module.cmake)

# Every doctest binary, every add_test, and the oracle suite hang off this one
# switch. Default is "am I the build" — a host embedding bronze wants the
# libraries and neither the test executables nor doctest as a dependency, and
# a bronze developer must never have to remember to turn its own ratchets on.
option(BRONZE_BUILD_TESTS "Build bronze's test binaries and register its ctest tests"
       ${BRONZE_IS_TOP_LEVEL})

if(BRONZE_BUILD_TESTS)
    enable_testing()
    find_package(doctest CONFIG REQUIRED)
endif()

add_subdirectory(src/abi)
add_subdirectory(src/support)
add_subdirectory(src/lex)
add_subdirectory(src/ast)
add_subdirectory(src/parse)
add_subdirectory(src/il)
add_subdirectory(src/json)
add_subdirectory(src/regex)
add_subdirectory(src/modules)
add_subdirectory(src/types)
add_subdirectory(src/lower)
add_subdirectory(src/codegen)
add_subdirectory(src/codegen-brass)
add_subdirectory(src/rt)
add_subdirectory(src/runtime)
add_subdirectory(src/embed)
add_subdirectory(src/cli)
# The runtime again, as a DLL/.so/.dylib. After every module it is built from,
# because it reads their source lists off the targets rather than relisting
# them. It needs no backend: the backend is what EMITS a loadable module, the
# runtime is only what one links against.
include(cmake/bronze_shared_runtime.cmake)

if(BRONZE_IS_TOP_LEVEL)
    add_subdirectory(tools)
endif()

if(BRONZE_BUILD_TESTS)
    add_subdirectory(tests/oracle)
    # Compiles JS to objects and links two of them into one host, so it needs
    # both the backend and the test registration — the same pair as the oracle.
    add_subdirectory(tests/two_module)
    # The same two-object build, entered from two THREADS at once: the
    # generated-code half of per-thread runtime isolation.
    add_subdirectory(tests/threaded_modules)
    # The same pair plus the shared runtime itself, which is what the module is
    # linked against and what the harness loads it into.
    if(BRONZE_BUILD_SHARED_RUNTIME)
        add_subdirectory(tests/shared_load)
        # The unload half of the same story: retire a loaded module, load its
        # replacement, one runtime throughout.
        add_subdirectory(tests/hot_swap)
    endif()
endif()

if(BRONZE_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_cabi_lowering.js")
    set(CABI_EXE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    if(MSVC)
        set(CABI_EXE_DIR "${CMAKE_CURRENT_BINARY_DIR}/Release")
    endif()
    add_custom_target(test_cabi_lowering
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CABI_EXE_DIR}"
        COMMAND $<TARGET_FILE:bronze> build "${CMAKE_CURRENT_SOURCE_DIR}/test_cabi_lowering.js" --native-manifest auto -o "${CABI_EXE_DIR}/test_cabi_lowering.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CABI_EXE_DIR}/test_cabi_lowering.exe" "${CMAKE_CURRENT_SOURCE_DIR}/test_cabi_lowering.exe"
        DEPENDS bronze
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Compiling test_cabi_lowering.js via bronze CLI"
    )
    if(BRONZE_BUILD_TESTS AND EXISTS "${CABI_EXE_DIR}/test_cabi_lowering.exe")
        add_test(NAME test_cabi_lowering COMMAND "${CABI_EXE_DIR}/test_cabi_lowering.exe")
        set_tests_properties(test_cabi_lowering PROPERTIES LABELS cabi)
    endif()
endif()


