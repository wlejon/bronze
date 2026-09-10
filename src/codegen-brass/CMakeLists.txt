set(BRASS_ROOT "/home/j/projects/brass" CACHE PATH "Path to Brass root directory")
if(DEFINED ENV{BRASS_ROOT})
    set(BRASS_ROOT "$ENV{BRASS_ROOT}")
endif()

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

target_link_libraries(bronze_codegen_brass PUBLIC
    bronze::abi
    bronze::codegen
    bronze::il
    bronze::support
    ${BRASS_ROOT}/build/libbrass.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
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
