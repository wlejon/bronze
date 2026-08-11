bronze_add_module(lower
    SOURCES lower.cpp lower_util.cpp lower_scope.cpp lower_stmt.cpp lower_control.cpp
            lower_expr.cpp lower_expr_cond.cpp lower_object.cpp assigned_set.cpp
    DEPS ast il lex parse support
    TEST_SOURCES ../../tests/lower/lower_test.cpp
)
