#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "codegen-brass/brass_backend.h"

TEST_CASE("brass backend reports name") {
    bronze::BrassBackend backend;
    CHECK(std::string(backend.name()) == "brass");
}
