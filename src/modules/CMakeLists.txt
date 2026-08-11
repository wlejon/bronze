bronze_add_module(modules
    SOURCES
        resolve.cpp
        graph.cpp
        link.cpp
        rename.cpp
    DEPS parse lex ast support
    TEST_SOURCES ../../tests/modules/modules_test.cpp
)
