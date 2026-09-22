bronze_add_module(modules
    SOURCES
        resolve.cpp
        import_map.cpp
        package.cpp
        graph.cpp
        link.cpp
        link_exports.cpp
        rename.cpp
    DEPS parse lex ast support json
    TEST_SOURCES ../../tests/modules/modules_test.cpp
)
