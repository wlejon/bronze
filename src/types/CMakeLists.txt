bronze_add_module(types
    SOURCES type.cpp shape_class.cpp class_layout.cpp class_field_types.cpp field_audit.cpp module_literal.cpp escape.cpp operator_types.cpp pins.cpp flow.cpp flow_expr.cpp flow_class_body.cpp method_ident.cpp ctor_ident.cpp infer.cpp result.cpp dump.cpp
    DEPS ast support
    TEST_SOURCES ../../tests/types/types_test.cpp ../../tests/types/field_audit_test.cpp ../../tests/types/method_param_test.cpp ../../tests/types/pins_test.cpp
    # Test-only: the fixtures need a parser to produce an AST. `bronze::types`
    # itself depends on nothing but ast + support.
    TEST_DEPS parse lex
)
