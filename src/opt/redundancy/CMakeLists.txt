bronze_add_module(opt_redundancy
    SOURCES bounds.cpp dead_code.cpp dead_stores.cpp dom_walk.cpp driver.cpp expr_table.cpp fold.cpp index_facts.cpp licence.cpp
            licm.cpp location.cpp strengthen.cpp tests.cpp value_walk.cpp
    DEPS il support opt_core
)
