#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

static std::string parseAndDump(std::string_view src) {
    SourceBuffer buf("t.ts", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = Parser(std::move(tokens), diags).parseModule("t");
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod != nullptr);
    return ast::dump(*mod);
}

TEST_CASE("function with typed params, if/else, calls") {
    const auto out = parseAndDump(
        "export function max(a: number, b: number): number {\n"
        "  if (a > b) { return a; } else { return b; }\n"
        "}\n"
        "const r = max(1, 2.5);\n");
    CHECK(out ==
          "(module t\n"
          "  (function max (a: number b: number): number exported\n"
          "    (if\n"
          "      (binary >\n"
          "        (ident a)\n"
          "        (ident b)\n"
          "      )\n"
          "      (then\n"
          "        (return\n"
          "          (ident a)\n"
          "        )\n"
          "      )\n"
          "      (else\n"
          "        (return\n"
          "          (ident b)\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          "  (const r\n"
          "    (call\n"
          "      (ident max)\n"
          "      (number 1)\n"
          "      (number 2.5)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("precedence: mul binds tighter than add, comparison loosest") {
    const auto out = parseAndDump("let x = 1 + 2 * 3 < 10;");
    CHECK(out.find("(binary <\n") != std::string::npos);
    // The + node must be the left child of <, and * the right child of +.
    CHECK(out ==
          "(module t\n"
          "  (let x\n"
          "    (binary <\n"
          "      (binary +\n"
          "        (number 1)\n"
          "        (binary *\n"
          "          (number 2)\n"
          "          (number 3)\n"
          "        )\n"
          "      )\n"
          "      (number 10)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("new expression with arguments") {
    const auto out = parseAndDump("const p = new Point(1, 2);");
    CHECK(out ==
          "(module t\n"
          "  (const p\n"
          "    (new Point\n"
          "      (number 1)\n"
          "      (number 2)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("new expression with zero arguments") {
    const auto out = parseAndDump("const p = new Foo();");
    CHECK(out ==
          "(module t\n"
          "  (const p\n"
          "    (new Foo\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("new without an argument list is a hard error") {
    const auto out = parseAndDump("const p = new Foo;");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("new requires an argument list") != std::string::npos);
}

TEST_CASE("new with a non-identifier callee is a hard error") {
    const auto out = parseAndDump("const p = new (getCtor())();");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("unsupported construct: new with a non-identifier callee") != std::string::npos);
}

TEST_CASE("const without initializer is a hard error") {
    const auto out = parseAndDump("const x;");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("initializer") != std::string::npos);
}

TEST_CASE("trailing garbage is a hard error, never dropped") {
    const auto out = parseAndDump("let x = 1; )");
    CHECK(out.substr(0, 7) == "ERRORS:");
}
