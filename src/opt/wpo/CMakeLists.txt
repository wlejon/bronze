bronze_add_module(wpo
    SOURCES value.cpp heap.cpp builtins.cpp solver.cpp solver_props.cpp solver_calls.cpp
            solver_builtins.cpp solver_summary.cpp shape.cpp census.cpp census_format.cpp stamp.cpp wpo.cpp
    DEPS il support
    TEST_SOURCES ../../../tests/opt/wpo_test.cpp
    # Test-only: the fixtures compile JavaScript to IL through the real front
    # end (tests/lower/lower_fixture.h).
    TEST_DEPS lex parse types lower
)
