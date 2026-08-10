#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "il/print.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"

using namespace bronze;

static std::optional<il::Module> parseAndLower(std::string_view src, DiagnosticSink& diags, SourceBuffer& buf) {
    buf = SourceBuffer("test.ts", std::string(src));
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return std::nullopt;
    auto astMod = Parser(std::move(tokens), diags).parseModule("test");
    if (diags.hasErrors() || !astMod) return std::nullopt;
    return lower::lowerModule(*astMod, diags);
}

TEST_CASE("numeric arithmetic, variables, and function calls") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function add(a: number, b: number): number {\n"
        "  return a + b;\n"
        "}\n"
        "export function calculate(x: number): number {\n"
        "  const doubled = x * 2;\n"
        "  const difference = doubled - 1;\n"
        "  const ratio = difference / 2;\n"
        "  return add(ratio, x);\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed ==
          "module test\n"
          "\n"
          "func add(%0: f64, %1: f64) -> f64 {\n"
          "  %2: f64 = add %0, %1\n"
          "  ret %2\n"
          "}\n"
          "\n"
          "func calculate(%0: f64) -> f64 export {\n"
          "  %1: f64 = const.f64 2\n"
          "  %2: f64 = mul %0, %1\n"
          "  %3: f64 = const.f64 1\n"
          "  %4: f64 = sub %2, %3\n"
          "  %5: f64 = const.f64 2\n"
          "  %6: f64 = div %4, %5\n"
          "  %7: f64 = call @add(%6, %0)\n"
          "  ret %7\n"
          "}\n");
}

TEST_CASE("numeric comparisons <, >, ==") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "export function compare(a: number, b: number): void {\n"
        "  const lt = a < b;\n"
        "  const gt = a > b;\n"
        "  const eq = a == b;\n"
        "  const seq = a === b;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed ==
          "module test\n"
          "\n"
          "func compare(%0: f64, %1: f64) -> void export {\n"
          "  %2: bool = cmp.lt %0, %1\n"
          "  %3: bool = cmp.gt %0, %1\n"
          "  %4: bool = cmp.eq %0, %1\n"
          "  %5: bool = cmp.eq %0, %1\n"
          "}\n");
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
          "  %0: f64 = const.f64 10\n"
          "  %1: f64 = const.f64 20\n"
          "  %2: f64 = add %0, %1\n"
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

TEST_CASE("unsupported AST node IfStmt generates diagnostic error") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function check(x: number): number {\n"
        "  if (x > 0) { return x; } else { return 0; }\n"
        "}\n",
        diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("unsupported AST node: IfStmt") != std::string::npos);
}

TEST_CASE("unsupported binary operator generates diagnostic error") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const ne = 1 != 2;\n",
        diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("unsupported binary operator: !=") != std::string::npos);
}

TEST_CASE("undefined variable reference generates diagnostic error") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const y = x + 1;\n",
        diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("undefined variable: x") != std::string::npos);
}
