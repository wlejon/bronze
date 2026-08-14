bronze_add_module(ast
    SOURCES ast.cpp assigned.cpp clone.cpp dump.cpp queries.cpp queries_declaration.cpp
            queries_tdz.cpp queries_yield.cpp yield_lift.cpp
    DEPS support
    TEST_SOURCES ../../tests/ast/queries_yield_test.cpp
    # Test-only: the queries answer questions about a tree, and a test needs a
    # lexer and parser to have one. `bronze::ast` itself defines the tree and
    # includes neither (see the note on TEST_DEPS in cmake/bronze_module.cmake).
    TEST_DEPS lex parse
)
