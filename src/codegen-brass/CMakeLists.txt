if(DEFINED ENV{BRASS_ROOT} AND NOT "$ENV{BRASS_ROOT}" STREQUAL "")
    set(BRASS_ROOT "$ENV{BRASS_ROOT}")
elseif(NOT DEFINED BRASS_ROOT OR BRASS_ROOT STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../brass/include/brass/brass.hpp")
        get_filename_component(BRASS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../brass" ABSOLUTE)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/../brass/include/brass/brass.hpp")
        get_filename_component(BRASS_ROOT "${CMAKE_SOURCE_DIR}/../brass" ABSOLUTE)
    elseif(EXISTS "D:/projects/brass/include/brass/brass.hpp")
        set(BRASS_ROOT "D:/projects/brass")
    else()
        get_filename_component(BRASS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../brass" ABSOLUTE)
    endif()
endif()
set(BRASS_ROOT "${BRASS_ROOT}" CACHE PATH "Path to Brass root directory" FORCE)

add_library(bronze_codegen_brass STATIC
    brass_backend.cpp
)
add_library(bronze::codegen_brass ALIAS bronze_codegen_brass)

target_include_directories(bronze_codegen_brass PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_include_directories(bronze_codegen_brass SYSTEM PUBLIC
    ${BRASS_ROOT}/include
)

find_package(Threads REQUIRED)

if(MSVC)
    if(EXISTS "${BRASS_ROOT}/build_msvc/brass.lib")
        set(BRASS_LIB "${BRASS_ROOT}/build_msvc/brass.lib")
    elseif(EXISTS "${BRASS_ROOT}/build/Release/brass.lib")
        set(BRASS_LIB "${BRASS_ROOT}/build/Release/brass.lib")
    elseif(EXISTS "${BRASS_ROOT}/build/brass.lib")
        set(BRASS_LIB "${BRASS_ROOT}/build/brass.lib")
    else()
        set(BRASS_LIB "${BRASS_ROOT}/build/libbrass.a")
    endif()
    set(PLATFORM_LIBS)
else()
    if(EXISTS "${BRASS_ROOT}/build/Release/libbrass.a")
        set(BRASS_LIB "${BRASS_ROOT}/build/Release/libbrass.a")
    elseif(EXISTS "${BRASS_ROOT}/build/libbrass.a")
        set(BRASS_LIB "${BRASS_ROOT}/build/libbrass.a")
    else()
        set(BRASS_LIB "${BRASS_ROOT}/build/brass.a")
    endif()
    set(PLATFORM_LIBS ${CMAKE_DL_LIBS} m)
endif()

target_link_libraries(bronze_codegen_brass PUBLIC
    bronze::abi
    bronze::codegen
    bronze::il
    bronze::support
    ${BRASS_LIB}
    Threads::Threads
    ${PLATFORM_LIBS}
)

if(MSVC)
    target_compile_options(bronze_codegen_brass PRIVATE /W4 /WX /permissive-)
    target_compile_definitions(bronze_codegen_brass PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(bronze_codegen_brass PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
endif()

if(BRONZE_BUILD_TESTS)
    add_executable(bronze_codegen_brass_tests
        ../../tests/codegen_brass/codegen_brass_test.cpp
    )
    target_link_libraries(bronze_codegen_brass_tests PRIVATE
        bronze::codegen_brass
        bronze::cli
        doctest::doctest
    )
    if(MSVC)
        target_compile_options(bronze_codegen_brass_tests PRIVATE /W4 /WX /permissive-)
        target_compile_definitions(bronze_codegen_brass_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(bronze_codegen_brass_tests PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs)
    endif()
    add_test(NAME codegen_brass COMMAND bronze_codegen_brass_tests)
    set_tests_properties(codegen_brass PROPERTIES LABELS codegen_brass)
endif()
