bronze_add_module(opt_repr
    SOURCES driver.cpp encoding.cpp fold_box.cpp params_apply.cpp params_solve.cpp params_truthy.cpp rewrite.cpp sink.cpp sink_allocs.cpp
    DEPS il support opt_core opt_redundancy
)
