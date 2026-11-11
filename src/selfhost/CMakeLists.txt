# The self-hosted builtins (README.md): builtins.js compiled by bronze's own
# compiler into one object every runtime image links.
#
# bronze_stage0 is the compiler for it, linked against a runtime WITHOUT the
# library (it is what makes it): the BRONZE_STAGE0 property below is what the
# runtime's link interface reads to leave the library out of exactly that one
# executable. Every other consumer of bronze_runtime — the CLI, the tests, a
# host embedding bronze — gets the library with the runtime, and the shared
# runtime (cmake/bronze_shared_runtime.cmake) has the object as a source.

# The library's text, compiled into the compiler (selfhost_library.h): what
# bronze_stage0 builds the prebuilt object from and what an ahead-of-time
# program compiles in (lower.h SelfHostLibrary). Editing builtins.js
# re-runs the configure step that writes it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/builtins.js" BRONZE_SELFHOST_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/builtins.js")
configure_file(selfhost_source.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/selfhost_source.cpp" @ONLY)
bronze_add_module(selfhost_il
    SOURCES selfhost_library.cpp "${CMAKE_CURRENT_BINARY_DIR}/selfhost_source.cpp"
    DEPS lower modules types support)

# The compiler's libraries one by one rather than bronze::cli, whose build
# depends on the runtimes (bronze_rt, bronze_host): this executable is
# upstream of both.
add_executable(bronze_stage0 stage0.cpp selfhost_build.cpp)
target_include_directories(bronze_stage0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bronze_stage0 PRIVATE bronze::parse bronze::lex bronze::ast bronze::support
    bronze::il bronze::types bronze::lower bronze::modules bronze::codegen_brass bronze::runtime bronze::embed
    bronze::selfhost_il)
set_target_properties(bronze_stage0 PROPERTIES BRONZE_STAGE0 ON)
if(MSVC)
    target_compile_options(bronze_stage0 PRIVATE /W4 /WX /permissive- /wd4324 /wd4702)
    target_compile_definitions(bronze_stage0 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(bronze_stage0 PRIVATE /FORCE:MULTIPLE)
endif()

set(BRONZE_SELFHOST_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/bronze_selfhost${CMAKE_CXX_OUTPUT_EXTENSION}"
    CACHE INTERNAL "The compiled self-hosted builtins")
add_custom_command(
    OUTPUT "${BRONZE_SELFHOST_OBJECT}"
    COMMAND bronze_stage0 "${BRONZE_SELFHOST_OBJECT}"
    DEPENDS bronze_stage0 "${CMAKE_CURRENT_SOURCE_DIR}/builtins.js"
    COMMENT "Compiling the self-hosted builtins"
    VERBATIM)
set_source_files_properties("${BRONZE_SELFHOST_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_library(bronze_selfhost STATIC "${BRONZE_SELFHOST_OBJECT}")
set_target_properties(bronze_selfhost PROPERTIES LINKER_LANGUAGE CXX)
# The library calls the runtime (its helpers, the ABI); the runtime calls the
# library's entry. A cycle of two archives, which CMake repeats on the link
# line for a single-pass linker.
target_link_libraries(bronze_selfhost INTERFACE bronze_runtime)
# Only an image that is LINKED takes it (an executable or a shared library;
# a static library in between would make every archive depend on the
# compiler that builds it), and never bronze_stage0.
target_link_libraries(bronze_runtime INTERFACE
    "$<$<AND:$<IN_LIST:$<TARGET_PROPERTY:TYPE>,EXECUTABLE;SHARED_LIBRARY;MODULE_LIBRARY>,$<NOT:$<BOOL:$<TARGET_PROPERTY:BRONZE_STAGE0>>>>:bronze_selfhost>")
