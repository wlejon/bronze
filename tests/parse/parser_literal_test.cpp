// Literals — `src/parse/parser_literal.cpp`'s half: what a numeric literal
// DENOTES once its radix prefix and separators are resolved, and the forms an
// object literal admits (shorthand, computed keys, method shorthand,
// accessors) with the home-object rule that comes with the last two.
//
// Regular expression literals are the same file's, and are in
// parser_regexp_test.cpp because they have a grammar of their own.

// The doctest main is parser_test.cpp's; every file here links into one
// binary under the `parse` label, so the module's test command does not
// change.

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

TEST_CASE("object shorthand is the key and the identifier, and computed keys dump apart") {
    // Two constructs that lower differently must not dump identically: a
    // written key is a constant, a computed one is an expression evaluated
    // before its value.
    const auto out = parseAndDump("const o = { x, [k]: 1 };\n");
    CHECK(out ==
          "(module t\n"
          "  (const o\n"
          "    (object\n"
          "      (prop x\n"
          "        (ident x)\n"
          "      )\n"
          "      (prop-computed\n"
          "        (ident k)\n"
          "        (number 1)\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("object method shorthand is an ordinary property holding a function") {
    // `{ m() {} }` and `{ m: function () {} }` define the same property, so
    // they dump the same way but for the function's IL SYMBOL. That symbol
    // carries dots deliberately: lowering registers every function it makes
    // under its name, and a method named `m` would otherwise answer a free
    // `m(...)` elsewhere in the module.
    const auto method = parseAndDump("const o = { m() { return 1; } };\n");
    CHECK(method ==
          "(module t\n"
          "  (const o\n"
          "    (object\n"
          "      (prop m\n"
          "        (function-expr obj.0.m ()\n"
          "          (return\n"
          "            (number 1)\n"
          "          )\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("a method's `super` belongs to its own home object, not the class around it") {
    // An object literal inside a class method is a different home object, so
    // `super` in one of its methods does not mean the enclosing class's
    // parent. bronze has no home-object model, so the honest answer is the
    // error — not a silent binding to the wrong parent.
    const auto out = parseAndDump(
        "class A extends B { m() { const o = { go() { return super.m(); } }; return o; } }\n");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("super outside a class method") != std::string::npos);
}

TEST_CASE("an object literal accessor is one property with two halves") {
    // The two halves of one name dump under separate heads, because that is
    // exactly what distinguishes them — `(prop x` twice would print `get x` and
    // `set x` identically.
    const auto out =
        parseAndDump("const o = { get x() { return 1; }, set x(v) { this.q = v; } };\n");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(prop-get x") != std::string::npos);
    CHECK(out.find("(prop-set x") != std::string::npos);

    // `get` is contextual: these three are ordinary properties and must keep
    // parsing.
    const auto plain = parseAndDump("const a = { get: 1 };\nconst get = 2;\nconst b = { get };\n");
    CHECK(plain.substr(0, 7) != "ERRORS:");

    // A string-literal accessor name is legal; a computed one is named.
    const auto strKey = parseAndDump("const o = { get \"a b\"() { return 1; } };\n");
    CHECK(strKey.substr(0, 7) != "ERRORS:");
    CHECK(strKey.find("(prop-get a b") != std::string::npos);
    const auto computed = parseAndDump("const o = { set [k](v) {} };\n");
    CHECK(computed.find("unsupported construct: a computed setter name") != std::string::npos);
}

TEST_CASE("numeric literal radix forms denote their digits") {
    // The dump renders the shortest text that round-trips (std::to_chars),
    // not the JS Number::toString of the value, so these are chosen to have
    // the same spelling under both — the claim under test is the VALUE a
    // literal denotes, and it should not move when the printer changes.
    const auto out = parseAndDump("const a = 0xFF, b = 0o17, c = 0b1010, d = 1_234_567;\n");
    CHECK(out.find("(number 255)") != std::string::npos);
    CHECK(out.find("(number 15)") != std::string::npos);
    CHECK(out.find("(number 10)") != std::string::npos);
    CHECK(out.find("(number 1234567)") != std::string::npos);
}

TEST_CASE("the dump of a number round-trips") {
    // Six significant digits was the old default and it lost this one, so a
    // dump could not distinguish two literals that are not the same number.
    const auto out = parseAndDump("const x = 123.4567;\n");
    CHECK(out.find("(number 123.4567)") != std::string::npos);
}

TEST_CASE("a legacy octal literal is a named error, not a guess") {
    // 017 is 15 read as octal and 17 read as decimal. ECMA-262 makes it a
    // strict-mode SyntaxError precisely because neither reading may be
    // assumed, and bronze says so rather than picking one.
    const auto out = parseAndDump("const x = 017;");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("legacy octal literal") != std::string::npos);

    // `08` is a NonOctalDecimalIntegerLiteral, the same production's other
    // half, and just as forbidden.
    const auto eight = parseAndDump("const x = 08;");
    CHECK(eight.substr(0, 7) == "ERRORS:");
    CHECK(eight.find("legacy octal literal") != std::string::npos);
}

TEST_CASE("a numeric separator must sit between two digits") {
    for (const char* src : {"const x = 1_;", "const x = 1__0;", "const x = 0x_ff;",
                            "const x = 1_.5;", "const x = 1._5;", "const x = 1e_5;"}) {
        const auto out = parseAndDump(src);
        CHECK(out.substr(0, 7) == "ERRORS:");
        CHECK(out.find("numeric separator '_' must appear between two digits") !=
              std::string::npos);
    }
}

TEST_CASE("a digit the radix does not have is a named error") {
    const auto out = parseAndDump("const x = 0b12;");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("invalid digit '2' in the binary literal") != std::string::npos);

    const auto bare = parseAndDump("const x = 0x;");
    CHECK(bare.substr(0, 7) == "ERRORS:");
    CHECK(bare.find("has no digits after its prefix") != std::string::npos);
}

TEST_CASE("a numeric property key parses as a computed property") {
    const auto out = parseAndDump("const o = { 0: 1 };");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(prop-computed") != std::string::npos);
}

TEST_CASE("the computed and string spellings of a numeric key still parse") {
    CHECK(parseAndDump("const o = { [0]: 1 };").substr(0, 7) != "ERRORS:");
    CHECK(parseAndDump("const o = { \"0\": 1 };").substr(0, 7) != "ERRORS:");
}

TEST_CASE("a method shorthand is flagged as one, and a function-valued property is not") {
    // The two forms define the SAME property and dump alike, which is what the
    // case above pins. The one thing that differs is 10.2.9's answer: a
    // shorthand method's `.name` is its property KEY, while `m: function g(){}`
    // really is named `g`. A shorthand's FunctionExpr carries the synthesized
    // IL symbol (`obj.0.m`) rather than an empty name, so nothing downstream
    // can tell the two apart from the function alone — hence the flag, and
    // hence this test, since the AST dump does not print it.
    SourceBuffer buf("t.ts", std::string("const o = { m() {}, p: function () {}, "
                                         "q: function g() {}, \"s\"() {}, [k]() {} };\n"));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = Parser(std::move(tokens), diags).parseModule("t");
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod != nullptr);

    REQUIRE(mod->body.size() == 1);
    const auto* decl = dynamic_cast<const ast::VarDecl*>(mod->body[0].get());
    REQUIRE(decl != nullptr);
    const auto* lit = dynamic_cast<const ast::ObjectLit*>(decl->init.get());
    REQUIRE(lit != nullptr);
    REQUIRE(lit->props.size() == 5);

    CHECK(lit->props[0].isMethod);        // { m() {} }
    CHECK_FALSE(lit->props[1].isMethod);  // { p: function () {} }
    CHECK_FALSE(lit->props[2].isMethod);  // { q: function g() {} }
    CHECK(lit->props[3].isMethod);        // { "s"() {} } — a string-literal key
    CHECK(lit->props[4].isMethod);        // { [k]() {} } — a computed one
    // And the flag really is about the SYNTAX rather than the value: every one
    // of these five holds a function expression.
    for (const auto& prop : lit->props) {
        CHECK(dynamic_cast<const ast::FunctionExpr*>(prop.value.get()) != nullptr);
    }
}
