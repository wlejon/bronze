bronze_add_module(runtime
    SOURCES
        fatal.cpp
        number_format.cpp
        value.cpp
        heap.cpp
        gc.cpp
        env.cpp
        string.cpp
        symbol.cpp
        builtin_symbol.cpp
        shape.cpp
        dictionary.cpp
        accessor.cpp
        exception.cpp
        object.cpp
        array.cpp
        typed_array.cpp
        builtin_typed_array.cpp
        builtin_typed_array_methods.cpp
        fn.cpp
        builtin_math.cpp
        builtin_array.cpp
        builtin_string.cpp
        builtin_function.cpp
        builtin_constructors.cpp
        inspect.cpp
        rt_state.cpp
        rt_convert.cpp
        rt_members.cpp
        rt_object.cpp
        rt_operator.cpp
        rt_prop.cpp
        rt_delete.cpp
        iterator.cpp
        map.cpp
        builtin_map.cpp
        builtin_object.cpp
        builtin_number.cpp
        builtin_number_proto.cpp
        builtin_json.cpp
        builtin_regexp.cpp
        builtin_string_regexp.cpp
        json_stringify.cpp
        exact_decimal.cpp
        rt_enumerate.cpp
        rt_spread.cpp
        rt_print.cpp
    DEPS
        abi
        json
        regex
    TEST_SOURCES
        ../../tests/runtime/number_format_test.cpp
        ../../tests/runtime/value_test.cpp
        ../../tests/runtime/gc_test.cpp
        ../../tests/runtime/string_test.cpp
        ../../tests/runtime/symbol_test.cpp
        ../../tests/runtime/shape_test.cpp
        ../../tests/runtime/object_test.cpp
        ../../tests/runtime/array_test.cpp
        ../../tests/runtime/typed_array_test.cpp
        ../../tests/runtime/fn_test.cpp
        ../../tests/runtime/inspect_test.cpp
        ../../tests/runtime/exception_test.cpp
        ../../tests/runtime/map_test.cpp
        ../../tests/runtime/exact_decimal_test.cpp
        ../../tests/runtime/regexp_test.cpp
        ../../tests/runtime/arguments_test.cpp
        ../../tests/runtime/math_random_test.cpp
)

# The same test binary, run again with per-allocation collection forced on.
# This surfaces unrooted-pointer-across-allocation bugs that a lazily
# collecting run can never catch.
add_test(NAME runtime-gc-stress COMMAND bronze_runtime_tests)
set_tests_properties(runtime-gc-stress PROPERTIES
    LABELS runtime
    ENVIRONMENT "BRONZE_GC_STRESS=1")
