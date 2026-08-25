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

// ---- the signature forms ----------------------------------------------------

TEST_CASE("a parameter pin names one position of one function") {
    const auto m = parsed(
        "param setBlending(blending): number\n"
        "param setBlending(src): number\n"
        "param Matrix4.multiplyMatrices(a): number\n");
    CHECK(m.size() == 3);
    CHECK(m.paramPinned("setBlending", "blending"));
    CHECK(m.paramPinned("setBlending", "src"));
    CHECK_FALSE(m.paramPinned("setBlending", "dst"));
    CHECK_FALSE(m.paramPinned("useProgram", "blending"));
}

TEST_CASE("a return pin names the function and nothing in it") {
    const auto m = parsed("return useProgram: number\n");
    CHECK(m.size() == 1);
    CHECK(m.returnPinned("useProgram"));
    CHECK_FALSE(m.returnPinned("setBlending"));
    CHECK_FALSE(m.paramPinned("useProgram", "program"));
}

// The linker renames every module-level binding into one namespace before
// inference runs, so the IL name of a method carries a module prefix the
// manifest was not written against.
TEST_CASE("a signature entry matches any dot-boundary suffix of the IL name") {
    const auto m = parsed(
        "param Matrix4.multiplyMatrices(a): number\n"
        "return Matrix4.multiplyMatrices: number\n");
    CHECK(m.paramPinned("mod1.Matrix4.multiplyMatrices", "a"));
    CHECK(m.returnPinned("mod1.Matrix4.multiplyMatrices"));
    // A same-named method on another class is a different function, and the
    // entry names the class precisely so the two can be told apart.
    CHECK_FALSE(m.paramPinned("mod1.Matrix3.multiplyMatrices", "a"));
    CHECK_FALSE(m.returnPinned("mod1.Matrix3.multiplyMatrices"));
}

// A suffix match must not let a general entry override a specific one: the
// first spelling that names the owner at all answers, hit or miss.
TEST_CASE("the most specific spelling of an owner is the one that answers") {
    const auto m = parsed(
        "param Matrix4.multiplyMatrices(a): number\n"
        "param multiplyMatrices(b): number\n");
    CHECK(m.paramPinned("mod1.Matrix4.multiplyMatrices", "a"));
    CHECK_FALSE(m.paramPinned("mod1.Matrix4.multiplyMatrices", "b"));
    CHECK(m.paramPinned("mod1.Matrix3.multiplyMatrices", "b"));
}

TEST_CASE("the signature forms live beside the field forms, not inside them") {
    const auto m = parsed(
        "Vector3.x: number\n"
        "param Vector3.setX(x): number\n");
    REQUIRE(m.lookup("Vector3", "x") != nullptr);
    // `setX` is not a field of Vector3, and `x` is not a parameter of Vector3.
    CHECK(m.lookup("Vector3", "setX") == nullptr);
    CHECK(m.paramPinned("Vector3.setX", "x"));
    CHECK_FALSE(m.paramPinned("Vector3", "x"));
}

TEST_CASE("a malformed signature line is named, never skipped") {
    CHECK(rejected("param setBlending: number\n").find("parameter pin target needs") !=
          std::string::npos);
    CHECK(rejected("param setBlending(x: number\n").find("parameter pin target needs") !=
          std::string::npos);
    CHECK(rejected("param setBlending(x): numeric-elements\n")
              .find("parameter pin's only kind") != std::string::npos);
    CHECK(rejected("return f: number-or-nullish\n").find("return pin's only kind") !=
          std::string::npos);
    CHECK(rejected("param 9f(x): number\n").find("not a valid function name") !=
          std::string::npos);
    CHECK(rejected("param f(9x): number\n").find("not a valid parameter name") !=
          std::string::npos);
    CHECK(rejected("return 9f: number\n").find("not a valid function name") !=
          std::string::npos);
}
