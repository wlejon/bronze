bronze_add_module(opt_redundancy
    SOURCES dead_code.cpp dead_stores.cpp dom_walk.cpp driver.cpp expr_table.cpp fold.cpp licence.cpp
            licm.cpp location.cpp tests.cpp value_walk.cpp
    DEPS il support opt_core
)
