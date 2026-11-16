bronze_add_module(ast
    SOURCES ast.cpp type.cpp assigned.cpp clone.cpp dump.cpp queries.cpp queries_declaration.cpp
            queries_tdz.cpp queries_yield.cpp
    DEPS support
)
