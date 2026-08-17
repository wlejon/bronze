# The host-facing embedding API. Depends on the runtime and only calls it:
# the host-global registry and the finalizer hook live IN src/runtime, and the
# runtime must never learn this module exists — the dependency arrow points
# here-to-there and nowhere else.
bronze_add_module(embed
    SOURCES
        embed.cpp
        embed_run.cpp
        embed_io.cpp
        embed_function.cpp
        embed_object.cpp
        embed_handle.cpp
        embed_typed_array.cpp
        embed_module.cpp
        embed_promise.cpp
    DEPS
        runtime
        abi
        support
    TEST_SOURCES
        ../../tests/embed/embed_test.cpp
)

# The same test binary, run again with per-allocation collection forced on —
# the runtime-gc-stress pattern. GC bugs in this module are systemic (a host
# holds values across frames, which is exactly what a lazily collecting run
# never exercises), so the stress run is the one that catches them.
if(BRONZE_BUILD_TESTS)
    add_test(NAME embed-gc-stress COMMAND bronze_embed_tests)
    set_tests_properties(embed-gc-stress PROPERTIES
        LABELS embed
        ENVIRONMENT "BRONZE_GC_STRESS=1")

    # And once more with the case order SHUFFLED, because every case in this
    # binary shares one process, one heap and one set of lazily built builtin
    # namespaces. A case that passes only because an earlier one warmed
    # something up passes forever in file order and fails the moment anything
    # reorders it — which is how a macOS CI run found a host-global case that
    # crashed under stress whenever it ran first. Order-independence is a
    # property of these tests, so it gets a test.
    #
    # A PINNED seed, so this is a ratchet and not a dice roll. Adding a case
    # reshuffles the permutation, which is the point rather than a problem:
    # the property under test is that no order is special.
    add_test(NAME embed-gc-stress-shuffled
             COMMAND bronze_embed_tests --order-by=rand --rand-seed=37)
    set_tests_properties(embed-gc-stress-shuffled PROPERTIES
        LABELS embed
        ENVIRONMENT "BRONZE_GC_STRESS=1")
endif()
