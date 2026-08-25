#include <doctest/doctest.h>

#include "types/pins.h"

using namespace bronze;
using namespace bronze::types;

namespace {

PinManifest parsed(const std::string& text) {
    PinManifest m;
    std::string err;
    REQUIRE_MESSAGE(m.parse(text, "test.pins", err), err);
    return m;
}

std::string rejected(const std::string& text) {
    PinManifest m;
    std::string err;
    CHECK_FALSE(m.parse(text, "test.pins", err));
    return err;
}

}  // namespace

// ---- the field forms --------------------------------------------------------

TEST_CASE("the three field kinds parse and are told apart") {
    const auto m = parsed(
        "Vector3.x: number\n"
        "Matrix4.elements: numeric-elements\n"
        "Light.distance: number-or-nullish\n");
    REQUIRE(m.size() == 3);
    REQUIRE(m.lookup("Vector3", "x") != nullptr);
    CHECK(*m.lookup("Vector3", "x") == PinKind::Number);
    CHECK(*m.lookup("Matrix4", "elements") == PinKind::NumericElements);
    CHECK(*m.lookup("Light", "distance") == PinKind::NumberOrNullish);
    CHECK(m.lookup("Vector3", "y") == nullptr);
}

TEST_CASE("a class is matched on the last dotted component") {
    const auto m = parsed("Vector3.x: number\n");
    CHECK(m.lookup("mod1.Vector3", "x") != nullptr);
}

TEST_CASE("an exact field entry wins over the class wildcard") {
    const auto m = parsed(
        "V.*: number\n"
        "V.opt: number-or-nullish\n");
    CHECK(*m.lookup("V", "anything") == PinKind::Number);
    CHECK(*m.lookup("V", "opt") == PinKind::NumberOrNullish);
}

// ---- the env-slot form ------------------------------------------------------

TEST_CASE("an env-slot pin is a separate namespace from the fields") {
    const auto m = parsed(
        "function WebGLState.currentBlending: number\n"
        "WebGLState.currentBlending: numeric-elements\n");
    CHECK(m.envSlotPinned("WebGLState", "currentBlending"));
    CHECK(m.envSlotPinned("mod2.WebGLState", "currentBlending"));
    CHECK_FALSE(m.envSlotPinned("WebGLState", "currentSrc"));
    CHECK_FALSE(m.envSlotPinned("Other", "currentBlending"));
    // The field entry of the same spelling is untouched by the env one.
    CHECK(*m.lookup("WebGLState", "currentBlending") == PinKind::NumericElements);
}

TEST_CASE("an env-slot pin admits no kind but number") {
    CHECK(rejected("function S.n: number-or-nullish\n")
              .find("env-slot pin's only kind is 'number'") != std::string::npos);
    CHECK(rejected("function S.n: numeric-elements\n")
              .find("env-slot pin's only kind is 'number'") != std::string::npos);
}

TEST_CASE("an env-slot pin admits no wildcard") {
    CHECK(rejected("function S.*: number\n")
              .find("not a valid binding name") != std::string::npos);
}

// ---- refusals ---------------------------------------------------------------
//
// Every one of these is a line that would otherwise read as "that field is not
// pinned", which is the one way a manifest may not fail.

TEST_CASE("a malformed line is named, never skipped") {
    CHECK(rejected("Vector3.x number\n").find("test.pins:1") != std::string::npos);
    CHECK(rejected("Vector3.x: numbr\n").find("unknown pin kind") != std::string::npos);
    CHECK(rejected("Vector3: number\n").find("pin target needs") != std::string::npos);
    CHECK(rejected("\n\nVector3.9x: number\n").find("test.pins:3") != std::string::npos);
}

TEST_CASE("comments and blank lines carry no entries") {
    const auto m = parsed("# a comment\n\n   \nV.x: number  # trailing\n");
    CHECK(m.size() == 1);
    CHECK(m.lookup("V", "x") != nullptr);
}

TEST_CASE("an empty manifest is empty in both namespaces") {
    PinManifest m;
    CHECK(m.empty());
    std::string err;
    CHECK(m.parse("# nothing\n", "test.pins", err));
    CHECK(m.empty());
}
