bronze_add_module(lower
    SOURCES lower.cpp
    DEPS ast il lex parse support
    TEST_SOURCES ../../tests/lower/lower_test.cpp
)
