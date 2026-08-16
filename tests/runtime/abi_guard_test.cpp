// The object-vs-runtime ABI guard (runtime/abi_guard.h): a matching stamp is
// silence, a mismatched one is a fatal naming both fingerprints. The real
// stamp lives in generated objects; these tests hand the check its value
// directly, which is exactly what the two program entries do.

#include <doctest/doctest.h>

#include <stdexcept>

#include "runtime/abi_guard.h"
#include "runtime/fatal.h"

using namespace bronze;
using namespace bronze::runtime;

TEST_CASE("abi guard: a matching object fingerprint passes silently") {
    rtCheckObjectAbi(BRONZE_ABI_FINGERPRINT);
}

TEST_CASE("abi guard: a mismatched object fingerprint is a named fatal") {
    setFatalHandler([](const char* msg) { throw std::runtime_error(msg); });
    CHECK_THROWS_WITH_AS(rtCheckObjectAbi(BRONZE_ABI_FINGERPRINT ^ 1u),
                         doctest::Contains("bronze ABI mismatch"), std::runtime_error);
    setFatalHandler(nullptr);
}
