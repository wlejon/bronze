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
        embed_native.cpp
        embed_typed_array.cpp
        embed_module.cpp
        embed_promise.cpp
        embed_realm.cpp
        embed_profiler.cpp
        embed_gc.cpp
        embed_clone.cpp
        embed_host_class.cpp
        embed_access.cpp
        embed_compartment.cpp
    DEPS
        runtime
        abi
        support
)
