// The env-slot number proof (`src/lower/lower_scope.cpp`
// `planEnvSlotNumberTypes`): which captured bindings a factory closure's
// environment record holds Numbers in, and therefore which reads of it stop
// testing the tag.
//
// Every case here is a fact about the PROOF, which no output can show — a
// program whose slot is unproven computes the same answers a proven one does,
// only through a checked unbox. The refusals matter more than the acceptance:
// each one is a program where the raw form would read a non-Number's bits as a
// double.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;

namespace {

// A slot's read is unboxed exactly when an `env.get` feeding it is followed by
// the raw form. Asked of the printed module because the two instructions are
// what the claim IS.
//
// Module-wide rather than per-name: an env read prints its record, depth and
// index, and the binding's NAME only while the read carries a dead-zone check —
// which the bindings here no longer do, their initializers being proven to run
// before anything can read them. Every case below is written to make the
// module-wide question the same one: the accepting cases capture exactly one
// binding, and the case that captures two asserts that NEITHER is raw.
bool anyReadIsRaw(const std::string& il) {
    size_t at = il.find("= env.get");
    while (at != std::string::npos) {
        const size_t lineEnd = il.find('\n', at);
        if (lineEnd == std::string::npos) return false;
        const size_t next = il.find('\n', lineEnd + 1);
        const std::string following =
            il.substr(lineEnd + 1, (next == std::string::npos ? il.size() : next) - lineEnd - 1);
        if (following.find("unbox.f64") != std::string::npos &&
            following.find(", raw") != std::string::npos) {
            return true;
        }
        at = il.find("= env.get", at + 1);
    }
    return false;
}

std::string lowerToText(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    return il::print(*mod);
}

}  // namespace

TEST_CASE("a captured counter written only from itself and literals is proven") {
    // The self-referential shape is the whole reason the proof is a fixpoint:
    // `n = n + 1` is provable only while `n` is itself assumed numeric, and a
    // single forward pass refuses every counter in the program.
    const auto il = lowerToText(
        "function make() {\n"
        "  let n = 0;\n"
        "  function bump() { n = n + 1; }\n"
        "  function read() { return n * 2; }\n"
        "  return [bump, read];\n"
        "}\n"
        "console.log(make());\n");
    CHECK(anyReadIsRaw(il));
}

TEST_CASE("a captured binding written from a parameter is refused") {
    // The env-slot pin case: nothing proves what a caller passes, so without a
    // manifest the read keeps its tag test.
    const auto il = lowerToText(
        "function make() {\n"
        "  let n = 0;\n"
        "  function set(v) { n = v; }\n"
        "  function read() { return n * 2; }\n"
        "  return [set, read];\n"
        "}\n"
        "console.log(make());\n");
    CHECK_FALSE(anyReadIsRaw(il));
}

TEST_CASE("one non-numeric write anywhere refuses the binding") {
    const auto il = lowerToText(
        "function make() {\n"
        "  let n = 0;\n"
        "  function bump() { n = n + 1; }\n"
        "  function wreck() { n = 'hi'; }\n"
        "  function read() { return n * 2; }\n"
        "  return [bump, wreck, read];\n"
        "}\n"
        "console.log(make());\n");
    CHECK_FALSE(anyReadIsRaw(il));
}

TEST_CASE("a nested binding of the same name refuses the outer one") {
    // The walk descends into nested functions and does not resolve scopes, so a
    // second binding of the name is a name this pass has not seen every write
    // to. Refusing is the honest answer; modelling the shadowing would be a
    // scope resolver.
    const auto il = lowerToText(
        "function make() {\n"
        "  let n = 0;\n"
        "  function bump() { n = n + 1; }\n"
        "  function other() { let n = 'hi'; return n; }\n"
        "  function read() { return n * 2; }\n"
        "  return [bump, other, read];\n"
        "}\n"
        "console.log(make());\n");
    CHECK_FALSE(anyReadIsRaw(il));
}

TEST_CASE("a var is refused where the same code as a let is proven") {
    // A `var` is initialized to `undefined` at function entry and its reads are
    // NOT dead-zone checked, so a closure called above the declaration reads
    // `undefined` out of the slot — which a raw unbox would take as a double.
    const std::string body =
        "function make() {\n"
        "  KIND n = 0;\n"
        "  function bump() { n = n + 1; }\n"
        "  function read() { return n * 2; }\n"
        "  return [bump, read];\n"
        "}\n"
        "console.log(make());\n";
    std::string asVar = body;
    asVar.replace(asVar.find("KIND"), 4, "var");
    std::string asLet = body;
    asLet.replace(asLet.find("KIND"), 4, "let");
    CHECK_FALSE(anyReadIsRaw(lowerToText(asVar)));
    CHECK(anyReadIsRaw(lowerToText(asLet)));
}

TEST_CASE("an uninitialized declaration is refused") {
    const auto il = lowerToText(
        "function make() {\n"
        "  let n;\n"
        "  function bump() { n = 1; }\n"
        "  function read() { return n * 2; }\n"
        "  return [bump, read];\n"
        "}\n"
        "console.log(make());\n");
    CHECK_FALSE(anyReadIsRaw(il));
}

TEST_CASE("a proven binding may not vouch for one that is refused") {
    // `a` survives; `b` takes a parameter and does not. The fixpoint descends,
    // so `a = a + b` must fall with `b` rather than standing on the round in
    // which `b` was still assumed numeric.
    const auto il = lowerToText(
        "function make() {\n"
        "  let a = 0;\n"
        "  let b = 0;\n"
        "  function set(v) { b = v; }\n"
        "  function bump() { a = a + b; }\n"
        "  function read() { return a * 2 + b * 3; }\n"
        "  return [set, bump, read];\n"
        "}\n"
        "console.log(make());\n");
    // Neither of them, which is what the module-wide form says here.
    CHECK_FALSE(anyReadIsRaw(il));
}
