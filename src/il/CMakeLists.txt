bronze_add_module(il
    SOURCES effects.cpp key.cpp print.cpp print_names.cpp serialize.cpp verifier.cpp verifier_proofs.cpp verifier_typed_entry.cpp
    DEPS support abi
    TEST_SOURCES ../../tests/il/il_test.cpp ../../tests/il/il_serialize_test.cpp ../../tests/il/verifier_proofs_test.cpp
)
