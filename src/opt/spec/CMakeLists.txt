bronze_add_module(spec
    SOURCES profile.cpp
    DEPS il support
    TEST_SOURCES ../../../tests/opt/spec_profile_test.cpp
    # Test-only: the key-stability cases compile JavaScript to IL through the
    # real front end (tests/lower/lower_fixture.h) and instrument it.
    TEST_DEPS lex parse types lower
)
