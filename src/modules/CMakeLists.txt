bronze_add_module(modules
    SOURCES
        resolve.cpp
        import_map.cpp
        package.cpp
        graph.cpp
        link.cpp
        link_exports.cpp
        link_types.cpp
        bundled_spread.cpp
        rename.cpp
        deps.cpp
    DEPS parse lex ast support json
)
