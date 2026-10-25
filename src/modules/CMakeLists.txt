bronze_add_module(modules
    SOURCES
        resolve.cpp
        import_map.cpp
        package.cpp
        graph.cpp
        link.cpp
        link_exports.cpp
        link_types.cpp
        rename.cpp
        deps.cpp
    DEPS parse lex ast support json
    TEST_SOURCES ../../tests/modules/modules_test.cpp ../../tests/modules/modules_types_test.cpp
)
