# Interface-only module today (backend.h); becomes a real library when the
# first shared lowering utilities land.
add_library(bronze_codegen INTERFACE)
add_library(bronze::codegen ALIAS bronze_codegen)
target_include_directories(bronze_codegen INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bronze_codegen INTERFACE bronze::il bronze::support)
