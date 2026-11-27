bronze_add_module(types
    SOURCES type.cpp shape_class.cpp class_layout.cpp class_capacity.cpp class_field_types.cpp field_audit.cpp field_audit_fields.cpp module_literal.cpp escape.cpp operator_types.cpp host_contracts.cpp relational_prover.cpp flow.cpp flow_expr.cpp flow_dispatch.cpp flow_construct.cpp flow_class_body.cpp method_ident.cpp ctor_ident.cpp infer.cpp infer_memo.cpp result.cpp dump.cpp
    DEPS ast support
)
