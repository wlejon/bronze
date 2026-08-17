// A module that is not one: the three symbols of bronze_abi.h's
// loadable-module contract, with the fingerprint deliberately wrong.
//
// This is the drift case made testable. The real failure it stands for is a
// host that keeps a module built by an older bronze and loads it against a
// newer runtime — the helper signatures have moved underneath it and the first
// call reads a parameter that was never passed. That failure is not
// reproducible in a test suite where everything is built together, and a
// fingerprint patched into the real module's bytes would be a test of where a
// constant lands in a .rdata section. A separate library that lies about its
// stamp is neither: it is exactly what the loader sees.
//
// The entry PRINTS. That is the assertion: the harness must refuse before
// calling it, so this line must never reach the pinned output. A refusal that
// happened after the entry ran would look identical on the refusal line alone.
//
// Nothing here links bronze. A module that cannot be entered never reaches the
// runtime, and a fake that needed the runtime to exist would be testing less
// than this one does.

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#define BRONZE_FAKE_EXPORT extern "C" __declspec(dllexport)
#else
#define BRONZE_FAKE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// The runtime's own fingerprint with every bit flipped, rather than a made-up
// constant: a literal could collide with the real value the day the header's
// hash happens to match it, and a test that passes because two numbers differ
// must not depend on luck for them to.
BRONZE_FAKE_EXPORT const uint32_t bronze_shared_demo_abi_fingerprint =
    static_cast<uint32_t>(BRONZE_ABI_FINGERPRINT) ^ 0xFFFFFFFFu;

// A well-formed manifest with no names: count 0 and nothing after it. The
// fake is wrong about exactly one thing, so that the refusal can only be
// about that one thing.
BRONZE_FAKE_EXPORT const unsigned char bronze_shared_demo_host_globals[4] = {0, 0, 0, 0};

BRONZE_FAKE_EXPORT void bronze_shared_demo() {
    std::printf("FAKE MODULE CODE RAN\n");
    std::fflush(stdout);
}
