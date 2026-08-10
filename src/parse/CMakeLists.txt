bronze_add_module(parse
    SOURCES parser.cpp
    DEPS lex ast
    TEST_SOURCES ../../tests/parse/parser_test.cpp
)
