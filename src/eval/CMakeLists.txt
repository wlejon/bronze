bronze_add_module(eval
    SOURCES
        eval.cpp
        eval_front.cpp
        aot_module.cpp
        code_cache.cpp
    DEPS
        lex
        parse
        ast
        types
        lower
        il
        opt_pipeline
        codegen_brass
        runtime
        embed
        support
        modules
        selfhost_il
)
# The code cache identifies its own binary with dladdr.
if(NOT WIN32)
    target_link_libraries(bronze_eval PUBLIC ${CMAKE_DL_LIBS})
endif()
