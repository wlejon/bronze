bronze_add_module(parse
    SOURCES parser.cpp
    # `support` is a direct dependency (parser.h includes
    # support/diagnostics.h), not one inherited through ast.
    DEPS lex ast support
    TEST_SOURCES ../../tests/parse/parser_test.cpp
)
