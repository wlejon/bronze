cmake_minimum_required(VERSION 3.24)

# vcpkg toolchain. Resolution order (same pattern as bro):
#   1. -DCMAKE_TOOLCHAIN_FILE=... on the command line (respected as-is)
#   2. $VCPKG_ROOT environment variable
#   3. Known machine locations (D:/vcpkg is where bro builds from)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
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

project(bronze VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Heavy dependencies are OPT-IN so the default configure/build/test loop
# never touches them. BRONZE_WITH_LLVM requires the vcpkg 'llvm' feature to
# have been provisioned (see vcpkg.json / docs/0001-foundation.md).
option(BRONZE_WITH_LLVM "Build the LLVM codegen backend" OFF)

include(cmake/bronze_module.cmake)

enable_testing()
find_package(doctest CONFIG REQUIRED)

add_subdirectory(src/support)
add_subdirectory(src/lex)
add_subdirectory(src/ast)
add_subdirectory(src/parse)
add_subdirectory(src/il)
add_subdirectory(src/codegen)
if(BRONZE_WITH_LLVM)
    add_subdirectory(src/codegen-llvm)
endif()
add_subdirectory(src/cli)
