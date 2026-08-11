bronze_add_module(parse
    SOURCES
        parser_stmt.cpp
        parser_expr.cpp
        parser_literal.cpp
        parser_func.cpp
        parser_pattern.cpp
        parser_module.cpp
    # `support` is a direct dependency (parser.h includes
    # support/diagnostics.h), not one inherited through ast.
    # `regex` is a dependency of the PARSER and not of lowering, because a
    # regular expression literal's body is compiled where it is written: the
    # pattern grammar is what decides whether the literal is well formed
    # (docs/0024 decision 3).
    DEPS lex ast support regex
    TEST_SOURCES ../../tests/parse/parser_test.cpp ../../tests/parse/parser_module_test.cpp
                 ../../tests/parse/parser_regexp_test.cpp
)
