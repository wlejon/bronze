bronze_add_module(check
    SOURCES static_checker.cpp static_checker_domain.cpp
    DEPS ast il lower modules support types
    TEST_SOURCES ../../tests/check/static_checker_test.cpp
    TEST_DEPS lex parse
)
