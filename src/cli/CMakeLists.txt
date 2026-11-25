add_library(bronze_cli STATIC driver.cpp link.cpp driver_helpers.cpp)
add_library(bronze::cli ALIAS bronze_cli)
target_include_directories(bronze_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(bronze_cli PUBLIC bronze::parse bronze::lex bronze::ast bronze::support bronze::il bronze::types bronze::lower bronze::check bronze::codegen bronze::modules bronze::codegen_brass bronze::opt_pipeline bronze::selfhost_il)
add_dependencies(bronze_cli bronze_rt)

if(MSVC)
    target_compile_options(bronze_cli PRIVATE /W4 /WX /permissive- /wd4324 /wd4702)
    target_compile_definitions(bronze_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(bronze main.cpp run.cpp ../host/cli_sys.cpp ../host/cli_proc.cpp)
target_link_libraries(bronze PRIVATE bronze::cli bronze::eval)
if(MSVC)
    target_compile_options(bronze PRIVATE /W4 /WX /permissive- /wd4324 /wd4702)
    target_compile_definitions(bronze PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(bronze PRIVATE /FORCE:MULTIPLE)
    # 8 MB main-thread stack, like bro's: the 1 MB default caps JS recursion
    # near 7,000 frames before the runtime's stack-limit check turns it
    # into a RangeError.
    target_link_options(bronze PRIVATE /STACK:8388608)
endif()
