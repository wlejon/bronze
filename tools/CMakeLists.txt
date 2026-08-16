# tools/CMakeLists.txt
#
# Standalone generator utilities. Built as small C++ targets with no heavy dependencies.

add_executable(gen_unicode_tables
    gen_unicode_tables.cpp
    gen_unicode_regex.cpp
    gen_unicode_runtime.cpp
)
target_include_directories(gen_unicode_tables PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set_target_properties(gen_unicode_tables PROPERTIES FOLDER "tools")
