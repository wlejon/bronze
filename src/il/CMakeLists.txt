bronze_add_module(il
    SOURCES effects.cpp key.cpp print.cpp serialize.cpp verifier.cpp
    DEPS support abi
    TEST_SOURCES ../../tests/il/il_test.cpp ../../tests/il/il_serialize_test.cpp
)
