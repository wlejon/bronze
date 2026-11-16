bronze_add_module(opt_speculate
    SOURCES driver.cpp evidence.cpp policy.cpp shape_key.cpp context.cpp resume_points.cpp guard_builder.cpp
            prop_shape.cpp prop_read.cpp prop_write.cpp construct.cpp direct_call.cpp method_call.cpp plain_call.cpp
            arith.cpp int_sources.cpp elem.cpp math_call.cpp loops.cpp
    DEPS il support opt_core spec
)
