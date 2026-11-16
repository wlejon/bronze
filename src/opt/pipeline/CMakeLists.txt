bronze_add_module(opt_pipeline
    SOURCES pipeline.cpp
    DEPS il support spec opt_speculate opt_inline opt_redundancy opt_repr
)
