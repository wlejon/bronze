bronze_add_module(eval
    SOURCES
        eval.cpp
        code_cache.cpp
    DEPS
        lex
        parse
        ast
        types
        lower
        il
        codegen_brass
        runtime
        embed
        support
        modules
    TEST_SOURCES
        ../../tests/eval/eval_test.cpp
        ../../tests/eval/eval_module_registry_test.cpp
        ../../tests/eval/eval_threads_test.cpp
        ../../tests/eval/eval_code_cache_test.cpp
)
# The code cache identifies its own binary with dladdr.
if(NOT WIN32)
    target_link_libraries(bronze_eval PUBLIC ${CMAKE_DL_LIBS})
endif()
