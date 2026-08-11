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

TEST_CASE("new expression is a receiver for member access and calls") {
    const auto out = parseAndDump("const s = new Point(1).scale(2).x;");
    CHECK(out ==
          "(module t\n"
          "  (const s\n"
          "    (member .x\n"
          "      (call\n"
          "        (member .scale\n"
          "          (new Point\n"
          "            (number 1)\n"
          "          )\n"
          "        )\n"
          "        (number 2)\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("arrow with a parenthesized parameter list and an expression body") {
    const auto out = parseAndDump("const f = (x) => x + 1;");
    CHECK(out ==
          "(module t\n"
          "  (const f\n"
          "    (arrow-expr <anon> (x)\n"
          "      (return\n"
          "        (binary +\n"
          "          (ident x)\n"
          "          (number 1)\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("arrow with a single bare parameter and a block body") {
    const auto out = parseAndDump("const f = x => { return x; };");
    CHECK(out ==
          "(module t\n"
          "  (const f\n"
          "    (arrow-expr <anon> (x)\n"
          "      (return\n"
          "        (ident x)\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("arrow on the right of an assignment, not just of a declaration") {
    // The arrow check lives at the operand entry point rather than in
    // parseExpr for exactly this: assignment is a binary operator here, so
    // its right side never passes back through parseExpr.
    const auto out = parseAndDump("this.get = () => 1;");
    CHECK(out.find("(arrow-expr <anon> ()") != std::string::npos);
    CHECK(out.substr(0, 7) != "ERRORS:");
}

TEST_CASE("for-of binds a name, an iterable and a body") {
    const auto out = parseAndDump("for (const x of a) { g(x); }");
    CHECK(out ==
          "(module t\n"
          "  (for-of x\n"
          "    (ident a)\n"
          "    (expr\n"
          "      (call\n"
          "        (ident g)\n"
          "        (ident x)\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("template literal alternates quasis and substitutions") {
    const auto out = parseAndDump("const t = `a${b}c`;");
    CHECK(out ==
          "(module t\n"
          "  (const t\n"
          "    (template\n"
          "      (quasi \"a\")\n"
          "      (ident b)\n"
          "      (quasi \"c\")\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("string escapes are decoded at parse time, not left raw") {
    const auto out = parseAndDump("const s = \"a\tb\u0041\";");
    CHECK(out.find("a\tbA") != std::string::npos);
}

TEST_CASE("a class body parses into methods, statics and a super call") {
    const auto out = parseAndDump(
        "class P extends Q {\n"
        "  constructor(x) { this.x = x; }\n"
        "  get() { return super.get(); }\n"
        "  static make() { return 1; }\n"
        "}\n");
    CHECK(out ==
          "(module t\n"
          "  (class P extends Q\n"
          "    (method constructor\n"
          "      (function-expr P.constructor (x)\n"
          "        (expr\n"
          "          (binary =\n"
          "            (member .x\n"
          "              (this)\n"
          "            )\n"
          "            (ident x)\n"
          "          )\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "    (method get\n"
          "      (function-expr P.get ()\n"
          "        (return\n"
          "          (call\n"
          "            (super-member Q.get)\n"
          "          )\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "    (static-method make\n"
          "      (function-expr P.make ()\n"
          "        (return\n"
          "          (number 1)\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("a base class with no constructor gets the empty one it has") {
    // Lowering wants exactly one constructor, always; the language says a
    // class that writes none has an empty one.
    const auto out = parseAndDump("class E {}");
    CHECK(out ==
          "(module t\n"
          "  (class E\n"
          "    (method constructor\n"
          "      (function-expr E.constructor ()\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("a derived class with no constructor is named, not forwarded") {
    // Forwarding needs rest and spread, and passing on fewer arguments than
    // the caller gave would be a wrong answer given quietly.
    const auto out = parseAndDump("class D extends B {}");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("a derived class with no constructor") != std::string::npos);
}

TEST_CASE("class members bronze has not built are named, not mis-parsed") {
    const auto field = parseAndDump("class C { x = 1; }");
    CHECK(field.find("unsupported construct: class field") != std::string::npos);

    const auto getter = parseAndDump("class C { get x() { return 1; } }");
    CHECK(getter.find("unsupported construct: class getter or setter") != std::string::npos);

    const auto computed = parseAndDump("class C { [k]() { return 1; } }");
    CHECK(computed.find("unsupported construct: computed method name") != std::string::npos);

    const auto gen = parseAndDump("class C { *each() { return 1; } }");
    CHECK(gen.find("unsupported construct: generator method") != std::string::npos);
}

TEST_CASE("super is legal only in a class method, and only with a parent") {
    const auto outside = parseAndDump("function f() { return super.m(); }");
    CHECK(outside.find("unsupported construct: super outside a class method") != std::string::npos);

    const auto noParent = parseAndDump("class C { m() { return super.m(); } }");
    CHECK(noParent.find("super in a class with no 'extends'") != std::string::npos);
}

TEST_CASE("`static` is still an ordinary name outside a class member position") {
    // It is not a reserved word in JavaScript, so taking it as a keyword
    // would have broken `obj.static` and `{ static: 1 }`.
    const auto out = parseAndDump("const o = { static: 1 }; const v = o.static;");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(member .static") != std::string::npos);
}

TEST_CASE("ES2015 parameter and spread syntax is named, not reported as punctuation") {
    struct Case {
        const char* src;
        const char* expected;
    };
    const Case cases[] = {
        {"function f(a, b = 2) {}", "unsupported construct: default parameter value"},
        {"const g = (a = 1) => a;", "unsupported construct: default parameter value"},
        {"function f(...r) {}", "unsupported construct: rest parameter"},
        {"class C { m(...r) {} }", "unsupported construct: rest parameter"},
        {"function h([a]) {}", "unsupported construct: destructuring parameter"},
        {"function h({a}) {}", "unsupported construct: destructuring parameter"},
        {"const [a, b] = [1, 2];", "unsupported construct: destructuring declaration"},
        {"const { x } = { x: 1 };", "unsupported construct: destructuring declaration"},
        {"f(...[1]);", "unsupported construct: spread"},
        {"const c = [...[1]];", "unsupported construct: spread"},
        {"const o = { ...x };", "unsupported construct: spread"},
    };
    for (const auto& c : cases) {
        const auto out = parseAndDump(c.src);
        CHECK(out.substr(0, 7) == "ERRORS:");
        CHECK(out.find(c.expected) != std::string::npos);
    }
}
