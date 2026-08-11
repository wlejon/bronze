bronze_add_module(lower
    SOURCES lower.cpp lower_infer.cpp lower_util.cpp lower_scope.cpp lower_stmt.cpp
            lower_control.cpp lower_expr.cpp lower_expr_cond.cpp lower_object.cpp
            assigned_set.cpp
    DEPS ast il support types
    TEST_SOURCES ../../tests/lower/lower_test.cpp
    # Test-only: the fixtures need a lexer and parser to produce an AST.
    # `bronze::lower` itself consumes an AST it is handed and includes
    # neither (see the note on TEST_DEPS in cmake/bronze_module.cmake).
    TEST_DEPS lex parse
)
