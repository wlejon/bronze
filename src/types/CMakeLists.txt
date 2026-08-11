bronze_add_module(types
    SOURCES type.cpp shape_class.cpp escape.cpp operator_types.cpp flow.cpp flow_expr.cpp infer.cpp result.cpp dump.cpp
    DEPS ast support
    TEST_SOURCES ../../tests/types/types_test.cpp
    # Test-only: the fixtures need a parser to produce an AST. `bronze::types`
    # itself depends on nothing but ast + support.
    TEST_DEPS parse lex
)
