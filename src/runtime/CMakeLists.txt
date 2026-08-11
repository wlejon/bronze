bronze_add_module(runtime
    SOURCES
        fatal.cpp
        number_format.cpp
        value.cpp
        heap.cpp
        gc.cpp
        env.cpp
        string.cpp
        shape.cpp
        object.cpp
        array.cpp
        typed_array.cpp
        fn.cpp
        builtin_math.cpp
        builtin_array.cpp
        builtin_string.cpp
        inspect.cpp
        rt_state.cpp
        rt_convert.cpp
        rt_members.cpp
        rt_object.cpp
        rt_operator.cpp
        rt_prop.cpp
        rt_iter.cpp
        rt_spread.cpp
        rt_print.cpp
    DEPS
        abi
    TEST_SOURCES
        ../../tests/runtime/number_format_test.cpp
        ../../tests/runtime/value_test.cpp
        ../../tests/runtime/gc_test.cpp
        ../../tests/runtime/string_test.cpp
        ../../tests/runtime/shape_test.cpp
        ../../tests/runtime/object_test.cpp
        ../../tests/runtime/array_test.cpp
        ../../tests/runtime/typed_array_test.cpp
        ../../tests/runtime/fn_test.cpp
        ../../tests/runtime/inspect_test.cpp
)

# The same test binary, run again with per-allocation collection forced on.
# This surfaces unrooted-pointer-across-allocation bugs that a lazily
# collecting run can never catch.
add_test(NAME runtime-gc-stress COMMAND bronze_runtime_tests)
set_tests_properties(runtime-gc-stress PROPERTIES
    LABELS runtime
    ENVIRONMENT "BRONZE_GC_STRESS=1")
