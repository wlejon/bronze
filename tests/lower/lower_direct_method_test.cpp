// The direct method-call edge (`src/lower/lower_infer.cpp`
// `resolveDirectMethodTargets`): which `method.call` sites name the function
// they will reach, and therefore which ones the backend may emit a real call
// to instead of an indirect one through the cache.
//
// The name is a GUESS. Nothing here proves that the receiver at run time
// resolves to the function named — the backend's compare against the cached
// code pointer is what decides that, and a wrong guess simply never matches.
// So the cases below are not about soundness of the target; they are about the
// two things a wrong answer WOULD cost: a site that could have been named and
// was not (a lost direct edge), and a site named with a callee whose operand
// list a fixed-arity call cannot express (a miscompile).

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;

namespace {

std::string lowerToText(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    return il::print(*mod);
}

// Whether any `method.call` in the module carries a named target. Asked of the
// printed module because `, direct @N` is what the claim IS — the field exists
// only to be read by the backend, and no other output distinguishes a site
// that has one from a site that does not.
bool hasDirectEdge(const std::string& il) {
    return il.find("direct @") != std::string::npos;
}

// How many of them do, for the cases where the count is the point.
size_t directEdges(const std::string& il) {
    size_t n = 0;
    size_t at = il.find("direct @");
    while (at != std::string::npos) {
        ++n;
        at = il.find("direct @", at + 1);
    }
    return n;
}

}  // namespace

TEST_CASE("a call on a receiver of known class names its method") {
    const auto il = lowerToText(
        "class Point {\n"
        "  constructor() { this.x = 1; }\n"
        "  get() { return this.x; }\n"
        "}\n"
        "function run() {\n"
        "  const p = new Point();\n"
        "  return p.get();\n"
        "}\n"
        "console.log(run());\n");
    CHECK(hasDirectEdge(il));
}

TEST_CASE("a call on a receiver of no single class names nothing") {
    // Two classes reach the same receiver, so inference widens it to a value
    // with no shape class and there is no table entry to look the method up
    // in. The site keeps the cache, which is the only thing that ever knew the
    // answer. (A receiver whose class inference DOES pin down is nameable even
    // when it arrives as a parameter — the name is a guess the backend
    // verifies, not a proof the lowering has to make.)
    const auto il = lowerToText(
        "class Point {\n"
        "  constructor() { this.x = 1; }\n"
        "  get() { return this.x; }\n"
        "}\n"
        "class Other {\n"
        "  constructor() { this.y = 2; }\n"
        "  get() { return this.y; }\n"
        "}\n"
        "function run(flag) {\n"
        "  const p = flag ? new Point() : new Other();\n"
        "  return p.get();\n"
        "}\n"
        "console.log(run(1));\n");
    CHECK_FALSE(hasDirectEdge(il));
}

TEST_CASE("an inherited method resolves to the base's declaration") {
    // The walk up `extends` is the up-half of the target set. `Derived` does
    // not declare `get`, so the name the site carries is `Base`'s — which is
    // exactly the function a receiver of class `Derived` reaches.
    const auto il = lowerToText(
        "class Base {\n"
        "  constructor() { this.x = 1; }\n"
        "  get() { return this.x; }\n"
        "}\n"
        "class Derived extends Base {\n"
        "  constructor() { super(); this.y = 2; }\n"
        "}\n"
        "function run() {\n"
        "  const d = new Derived();\n"
        "  return d.get();\n"
        "}\n"
        "console.log(run());\n");
    CHECK(hasDirectEdge(il));
}

TEST_CASE("an override is named by the nearest declaration, not the base's") {
    // Both sites are nameable and they must not be named the same function.
    // Nothing in the printed form says which index is which class, so the
    // assertion is the one the bug would break: two sites, two DIFFERENT
    // targets. A walk that stopped at the base would print the same index
    // twice and hand the backend a code pointer that never matches.
    const auto il = lowerToText(
        "class Base {\n"
        "  constructor() { this.x = 1; }\n"
        "  get() { return this.x; }\n"
        "}\n"
        "class Derived extends Base {\n"
        "  constructor() { super(); }\n"
        "  get() { return this.x + 1; }\n"
        "}\n"
        "function run() {\n"
        "  const b = new Base();\n"
        "  const d = new Derived();\n"
        "  return b.get() + d.get();\n"
        "}\n"
        "console.log(run());\n");
    REQUIRE(directEdges(il) == 2);
    const size_t first = il.find("direct @");
    const size_t second = il.find("direct @", first + 1);
    const std::string a = il.substr(first, il.find('\n', first) - first);
    const std::string b = il.substr(second, il.find('\n', second) - second);
    CHECK(a != b);
}

TEST_CASE("a callee that reads arguments is refused") {
    // The `arguments` object is built from a count only the wrapper sees. A
    // fixed operand list cannot supply it, so the site stays on the boxed path
    // even though the receiver's class is known.
    const auto il = lowerToText(
        "class Point {\n"
        "  constructor() { this.x = 1; }\n"
        "  get(a) { return arguments.length + this.x; }\n"
        "}\n"
        "function run() {\n"
        "  const p = new Point();\n"
        "  return p.get(1);\n"
        "}\n"
        "console.log(run());\n");
    CHECK_FALSE(hasDirectEdge(il));
}

TEST_CASE("a callee with a rest parameter is refused") {
    const auto il = lowerToText(
        "class Point {\n"
        "  constructor() { this.x = 1; }\n"
        "  get(...rest) { return rest.length + this.x; }\n"
        "}\n"
        "function run() {\n"
        "  const p = new Point();\n"
        "  return p.get(1, 2);\n"
        "}\n"
        "console.log(run());\n");
    CHECK_FALSE(hasDirectEdge(il));
}

TEST_CASE("a call passing more arguments than the callee declares is refused") {
    // The extra operands have nowhere to go in a fixed list, and dropping them
    // would be a different program: the callee could still observe them if it
    // ever grew an `arguments` read.
    const auto il = lowerToText(
        "class Point {\n"
        "  constructor() { this.x = 1; }\n"
        "  get(a) { return a + this.x; }\n"
        "}\n"
        "function run() {\n"
        "  const p = new Point();\n"
        "  return p.get(1, 2, 3);\n"
        "}\n"
        "console.log(run());\n");
    CHECK_FALSE(hasDirectEdge(il));
}

TEST_CASE("a method no class in the table declares names nothing") {
    // `toFixed` is a builtin: the receiver's class is known and the method is
    // real, but no lowered function implements it, so there is no index to
    // name. The refusal is the table's, not the guard's.
    const auto il = lowerToText(
        "class Point {\n"
        "  constructor() { this.x = 1; }\n"
        "  get() { return this.x; }\n"
        "}\n"
        "function run() {\n"
        "  const p = new Point();\n"
        "  return p.toString();\n"
        "}\n"
        "console.log(run());\n");
    CHECK_FALSE(hasDirectEdge(il));
}
