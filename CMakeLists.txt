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

option(BRONZE_WITH_LLVM "Build the LLVM codegen backend" OFF)
if(BRONZE_WITH_LLVM)
    if(NOT DEFINED LLVM_DIR AND NOT DEFINED ENV{LLVM_DIR})
        foreach(_llvm_candidate
                "D:/llvm-22/lib/cmake/llvm"
                "D:/llvm-21/lib/cmake/llvm"
                "D:/llvm-20/lib/cmake/llvm"
                "C:/Program Files/LLVM/lib/cmake/llvm"
                "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows/share/llvm"
                "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/arm64-osx/share/llvm"
                "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-osx/share/llvm"
                "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-linux/share/llvm"
                "/opt/homebrew/opt/llvm/lib/cmake/llvm"
                "/usr/local/opt/llvm/lib/cmake/llvm"
                "/usr/lib/llvm-22/lib/cmake/llvm"
                "/usr/lib/llvm-22/cmake"
                "/usr/lib/llvm-21/lib/cmake/llvm"
                "/usr/lib/llvm-21/cmake"
                "/usr/lib/llvm-20/lib/cmake/llvm"
                "/usr/lib/llvm-20/cmake")
            if(EXISTS "${_llvm_candidate}")
                set(LLVM_DIR "${_llvm_candidate}" CACHE PATH "" FORCE)
                set(LLVM_DIR "${_llvm_candidate}")
                list(APPEND CMAKE_PREFIX_PATH "${_llvm_candidate}/../..")
                break()
            endif()
        endforeach()
    endif()
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
if(BRONZE_WITH_LLVM)
    add_subdirectory(src/codegen-llvm)
endif()
add_subdirectory(src/rt)
add_subdirectory(src/runtime)
add_subdirectory(src/embed)
add_subdirectory(src/cli)

if(BRONZE_IS_TOP_LEVEL)
    add_subdirectory(tools)
endif()

if(BRONZE_WITH_LLVM AND BRONZE_BUILD_TESTS)
    add_subdirectory(tests/oracle)
endif()

