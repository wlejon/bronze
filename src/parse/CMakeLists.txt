bronze_add_module(parse
    SOURCES
        parser_stmt.cpp
        parser_expr.cpp
        parser_literal.cpp
        parser_func.cpp
        parser_class.cpp
        parser_generator.cpp
        parser_async.cpp
        parser_pattern.cpp
        parser_module.cpp
        parser_strict.cpp
        parser_type.cpp
        parser_type_members.cpp
        parser_type_lookahead.cpp
        parser_ts.cpp
        parser_ts_enum.cpp
        parser_ts_namespace.cpp
        parser_ts_params.cpp
        parser_ts_rewrite.cpp
    # `support` is a direct dependency (parser.h includes
    # support/diagnostics.h), not one inherited through ast.
    # `regex` is a dependency of the PARSER and not of lowering, because a
    # regular expression literal's body is compiled where it is written: the
    # pattern grammar is what decides whether the literal is well formed
    # itself.
    DEPS lex ast support regex
)
