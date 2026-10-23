if(MSVC)
    set(_disc_warnings /W4 /WX /permissive- /wd4324 /utf-8)
else()
    set(_disc_warnings -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-trigraphs -fno-omit-frame-pointer)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/discovery_codec_test.cpp")
    add_executable(bronze_discovery_codec_tests discovery_codec_test.cpp)
    target_link_libraries(bronze_discovery_codec_tests PRIVATE bronze::check bronze::lex bronze::parse doctest::doctest)
    target_compile_options(bronze_discovery_codec_tests PRIVATE ${_disc_warnings})
    add_test(NAME discovery-codec COMMAND bronze_discovery_codec_tests)
    set_tests_properties(discovery-codec PROPERTIES LABELS "discovery;check")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/discovery_math_test.cpp")
    add_executable(bronze_discovery_math_tests discovery_math_test.cpp)
    target_link_libraries(bronze_discovery_math_tests PRIVATE bronze::check bronze::lex bronze::parse doctest::doctest)
    target_compile_options(bronze_discovery_math_tests PRIVATE ${_disc_warnings})
    add_test(NAME discovery-math COMMAND bronze_discovery_math_tests)
    set_tests_properties(discovery-math PROPERTIES LABELS "discovery;check")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/discovery_stdlib_test.cpp")
    add_executable(bronze_discovery_stdlib_tests discovery_stdlib_test.cpp)
    target_link_libraries(bronze_discovery_stdlib_tests PRIVATE bronze::check bronze::lex bronze::parse doctest::doctest)
    target_compile_options(bronze_discovery_stdlib_tests PRIVATE ${_disc_warnings})
    add_test(NAME discovery-stdlib COMMAND bronze_discovery_stdlib_tests)
    set_tests_properties(discovery-stdlib PROPERTIES LABELS "discovery;check")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/discovery_systems_test.cpp")
    add_executable(bronze_discovery_systems_tests discovery_systems_test.cpp)
    target_link_libraries(bronze_discovery_systems_tests PRIVATE bronze::check bronze::lex bronze::parse doctest::doctest)
    target_compile_options(bronze_discovery_systems_tests PRIVATE ${_disc_warnings})
    add_test(NAME discovery-systems COMMAND bronze_discovery_systems_tests)
    set_tests_properties(discovery-systems PROPERTIES LABELS "discovery;check")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/discovery_physics_shader_test.cpp")
    add_executable(bronze_discovery_physics_shader_tests discovery_physics_shader_test.cpp)
    target_link_libraries(bronze_discovery_physics_shader_tests PRIVATE
        bronze::codegen_brass
        bronze::runtime
        bronze::check
        bronze::lex
        bronze::parse
        doctest::doctest
    )
    target_compile_options(bronze_discovery_physics_shader_tests PRIVATE ${_disc_warnings})
    add_test(NAME discovery-physics-shader COMMAND bronze_discovery_physics_shader_tests)
    set_tests_properties(discovery-physics-shader PROPERTIES LABELS "discovery;check")
endif()
