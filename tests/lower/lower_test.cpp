// The module skeleton and the expression and statement forms that do not
// have a seam of their own yet: literals, property access and calls,
// if/switch, for-of, classes and `super`, binding patterns, and the
// operator families of docs/0015. The units named for one seam each —
// inference, resolution, scopes, jumps, try — live in their own files.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

TEST_CASE("numeric comparisons <, >, ==") {
    // Unannotated, and proven numeric by the call site instead — which is
    // the point: the comparisons lower to native `cmp.*` because inference
    // proved the operands, not because anyone wrote `: number`.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function compare(a, b) {\n"
        "  const lt = a < b;\n"
        "  const gt = a > b;\n"
        "  const eq = a == b;\n"
        "  const seq = a === b;\n"
        "  return lt;\n"
        "}\n"
        "console.log(compare(1, 2));\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed.find(
              "func compare(%0: f64, %1: f64) -> bool {\n"
              "  b0:\n"
              "    %2: bool = cmp.lt %0, %1\n"
              "    %3: bool = cmp.gt %0, %1\n"
              "    %4: bool = cmp.eq %0, %1\n"
              "    %5: bool = cmp.eq %0, %1\n"
              "    ret %2\n"
              "}\n") != std::string::npos);
}

TEST_CASE("top-level statements lowered to main") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const x = 10;\n"
        "const y = 20;\n"
        "const z = x + y;\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed ==
          "module test\n"
          "\n"
          "func main() -> void {\n"
          "  b0:\n"
          "    %0: f64 = const.f64 10\n"
          "    %1: f64 = const.f64 20\n"
          "    %2: f64 = add %0, %1\n"
          "    ret\n"
          "}\n");
}

TEST_CASE("string literal lowered to box.str") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const s = \"hello\";\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("%0: dynamic = box.str") != std::string::npos);
}

TEST_CASE("lowering dynamic types, property access, array indexing, and dynamic calls") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "export function testDynamic(obj: dynamic, fn: dynamic): dynamic {\n"
        "  obj.prop = 42;\n"
        "  const val = obj.prop;\n"
        "  const elem = obj[0];\n"
        "  return fn(val, elem);\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed.find("prop.set %0") != std::string::npos);
    CHECK(printed.find("prop.get %0") != std::string::npos);
    CHECK(printed.find("call.dynamic %1") != std::string::npos);
}

TEST_CASE("IfStmt lowers to branch and join blocks") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function check(x: number): number {\n"
        "  if (x > 0) { return x; } else { return 0; }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("br %") != std::string::npos);
}

TEST_CASE("comparison != lowers to cmp.ne") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const ne = 1 != 2;\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("cmp.ne") != std::string::npos);
}

TEST_CASE("switch lowers to a strict-equality test chain") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "switch (1) { case 1: break; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // ECMA-262 14.12.10 matches with IsStrictlyEqual, never with the loose
    // rules, so a `cmp.eq` here would be a wrong answer and not a style
    // choice (docs/0018 decision 4).
    CHECK(printed.find("strict.eq") != std::string::npos);
    CHECK(printed.find("br %") != std::string::npos);
}

TEST_CASE("for-of opens an iterator and closes it when the body throws") {
    // docs/0021 decision 2: the loop is a cursor, not an index and a length
    // — an iterator is opened once, stepped, and read. And decision 3: the
    // body runs under a handler whose only job is to close the iterator and
    // re-raise, so a `throw` from inside the loop still reaches the
    // iterator's `return` method.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const a = [1];\n"
        "let s = 0;\n"
        "for (const x of a) { s = s + x; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("iter.open") != std::string::npos);
    CHECK(printed.find("iter.step") != std::string::npos);
    CHECK(printed.find("iter.value") != std::string::npos);
    // Suppressed, because a throw is already on its way out and 7.4.9 step 6
    // keeps that one rather than anything `return` might raise.
    CHECK(printed.find("iter.close %4, suppress") != std::string::npos);
    CHECK(printed.find("exc.take") != std::string::npos);
    // No index is threaded through the loop's blocks any more: the record
    // holds the cursor, so the only block parameters are source bindings.
    CHECK(printed.find("iter.advance") == std::string::npos);
}

TEST_CASE("a class lowers to a constructor, a prototype and property writes") {
    // No new runtime concept: the class IS the constructor function, its
    // methods are stored on `.prototype`, and a static is stored on the
    // function itself (docs/0012 decisions 5 and 6).
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "class Q { constructor() { this.v = 1; } get() { return this.v; } }\n"
        "class P extends Q {\n"
        "  constructor() { super(); }\n"
        "  get() { return super.get(); }\n"
        "  static make() { return 1; }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func Q(%0: dynamic, %1: dynamic)") != std::string::npos);
    CHECK(printed.find("func Q.get(%0: dynamic, %1: dynamic)") != std::string::npos);
    CHECK(printed.find("func P.make(%0: dynamic)") != std::string::npos);
    // `extends` runs before any method is stored: the prototype object it
    // installs is the one they have to land on.
    const size_t extend = printed.find("class.extend");
    const size_t protoRead = printed.find("prop.get %5, 1", extend);
    REQUIRE(extend != std::string::npos);
    CHECK(protoRead != std::string::npos);
}

TEST_CASE("super.m() calls the parent's method with the current receiver") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "class Q { constructor() { this.v = 1; } get() { return this.v; } }\n"
        "class P extends Q { constructor() { super(); } get() { return super.get(); } }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    const size_t body = printed.find("func P.get(");
    REQUIRE(body != std::string::npos);
    // Two reads (the parent, then its prototype, then the method) and a
    // dynamic call whose receiver is %1 — the CURRENT `this`, not the
    // parent prototype the function came from. Reading `this.get` instead
    // would find the override and recurse forever.
    CHECK(printed.find("call.dynamic %4, %1", body) != std::string::npos);
}

TEST_CASE("a method with no `this` in it still gets a receiver when it uses super") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "class Q { constructor() { this.v = 1; } tag() { return \"q\"; } }\n"
        "class P extends Q { constructor() { super(); } tag() { return super.tag(); } }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func P.tag(%0: dynamic, %1: dynamic)") != std::string::npos);
}

TEST_CASE("a destructuring assignment reads the whole right side before writing") {
    // `[a, b] = [b, a]` is a swap only if this holds. The array is built
    // first, and every `iter.at` reads THAT array — so no target write can be
    // seen by a later read (docs/0017 decision 4).
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("let a = 1;\nlet b = 2;\n[a, b] = [b, a];\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // The source is checked once, up front, so a later element read cannot
    // blame for-of for a destructuring failure.
    CHECK(printed.find("pattern.check %2, array") != std::string::npos);
    // One iterator over that array, stepped once per element, and closed at
    // the end because the pattern stopped before the source was exhausted
    // (ECMA-262 8.6.2 step 5). Every read names the SAME record, which is
    // what makes the swap a swap.
    CHECK(printed.find("iter.open %5") != std::string::npos);
    CHECK(printed.find("%8: bool = iter.step %6") != std::string::npos);
    CHECK(printed.find("%7: dynamic = iter.value %6") != std::string::npos);
    CHECK(printed.find("%10: bool = iter.step %6") != std::string::npos);
    CHECK(printed.find("%9: dynamic = iter.value %6") != std::string::npos);
    CHECK(printed.find("iter.close %6, abrupt") != std::string::npos);
}

TEST_CASE("a default parameter is a branch, and only undefined takes it") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function f(a, b = a * 2) { return b; }\nf(1);\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // A strict comparison against undefined, not a truthiness test: `null`,
    // `0` and `""` are arguments that were passed (docs/0017 decision 1).
    CHECK(printed.find("const.undefined") != std::string::npos);
    CHECK(printed.find("strict.eq") != std::string::npos);
    // A BRANCH, not a select — the default's own code runs only when it
    // fires, so its side effects fire with it.
    CHECK(printed.find("br ") != std::string::npos);
    // The default sees the parameters to its left: `a * 2` reads `a`.
    CHECK(printed.find("mul") != std::string::npos);
}

TEST_CASE("a rest parameter is not an argument the caller passes") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function count(first, ...rest) { return rest; }\ncount(1, 2, 3);\n",
                      diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // Two IL parameters, because the rest one is still a value the body
    // reads — but the call site builds it rather than passing three
    // arguments, which is what keeps the arity fixed (docs/0017 decision 2).
    CHECK(printed.find("func count(%0: dynamic, %1: dynamic)") != std::string::npos);
    // The leftovers are appended into an array that starts EMPTY: `create.array
    // n` makes n undefined elements, so a non-zero length here would leave the
    // rest array with two undefineds in front of what the call passed.
    CHECK(printed.find("create.array 0") != std::string::npos);
    CHECK(printed.find("array.append") != std::string::npos);
}

TEST_CASE("a spread call passes one array, and a spread literal appends") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function g(x, y) { return x; }\nconst a = [1, 2];\n"
                      "const b = [0, ...a, 3];\nconst f = g;\nf(...a);\n",
                      diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("array.spread") != std::string::npos);
    CHECK(printed.find("call.dynamic.spread") != std::string::npos);
}

TEST_CASE("a bitwise operator is int32 inside and f64 outside") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("const a = 5 & 3;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // Each operand converts once, and the RESULT is the JS number the int32
    // denotes — an i32 escaping the operator would be a type inference has
    // no element for (docs/0015 decision 1).
    CHECK(printed.find(": i32 = to.int32") != std::string::npos);
    CHECK(printed.find(": f64 = and") != std::string::npos);
}

TEST_CASE("`~x` is a xor against -1, not an op of its own") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("const a = ~5;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("const.i32 -1") != std::string::npos);
    CHECK(printed.find(": f64 = xor") != std::string::npos);
}

TEST_CASE("numeric truthiness is num.truthy, not cmp.ne against zero") {
    // The two differ at exactly one value: `if (NaN)` is false while
    // `NaN !== NaN` is true, so one op cannot answer both (docs/0015
    // decision 9).
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function f(n: number): number { if (n) { return 1; } return 0; }\n"
        "f(1);\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("num.truthy") != std::string::npos);
}

TEST_CASE("loose equality on unproven operands lowers to the runtime algorithm") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("const a = 1 == \"1\";\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("loose.eq") != std::string::npos);
}

TEST_CASE("loose equality on two proven numbers is the same compare as strict") {
    // Not an optimization on top of IsLooselyEqual — it is that algorithm's
    // first step, and it is what makes `==` free in typed code.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("const a = 1 == 2;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("cmp.eq") != std::string::npos);
    CHECK(printed.find("loose.eq") == std::string::npos);
}
