# The host-facing embedding API. Depends on the runtime and only calls it:
# the host-global registry and the finalizer hook live IN src/runtime, and the
# runtime must never learn this module exists — the dependency arrow points
# here-to-there and nowhere else.
bronze_add_module(embed
    SOURCES
        embed.cpp
        embed_run.cpp
        embed_function.cpp
        embed_object.cpp
        embed_handle.cpp
        embed_typed_array.cpp
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
add_test(NAME embed-gc-stress COMMAND bronze_embed_tests)
set_tests_properties(embed-gc-stress PROPERTIES
    LABELS embed
    ENVIRONMENT "BRONZE_GC_STRESS=1")
