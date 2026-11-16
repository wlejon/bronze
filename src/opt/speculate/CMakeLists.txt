bronze_add_module(opt_speculate
    SOURCES driver.cpp evidence.cpp policy.cpp context.cpp resume_points.cpp guard_builder.cpp
            prop_shape.cpp shape_sets.cpp prop_read.cpp prop_slot.cpp prop_write.cpp construct.cpp direct_call.cpp method_call.cpp plain_call.cpp
            arith.cpp int_sources.cpp elem.cpp tdz.cpp math_call.cpp loops.cpp evidence_member.cpp member_call.cpp
    DEPS il support opt_core spec
)
