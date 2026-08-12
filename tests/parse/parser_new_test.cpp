// The `new` production and nothing else: ECMA-262 13.3's
//
//   MemberExpression : new MemberExpression Arguments
//   NewExpression    : MemberExpression | new NewExpression
//
// which is one grammar rule with four grouping consequences, each of which is
// a different TREE for the same tokens. Every case here is written so that a
// wrong grouping produces a visibly different dump rather than a plausible
// one — `new a.b.c()` and `new a.b().c` are the pair that catches a parser
// which stops the callee at the wrong place.
//
// Whether the groupings are also EVALUATED correctly is
// tests/oracle/cases/new_callee_*, because a tree assertion passes just as
// happily when the parser is consistently wrong.

// The doctest main is parser_test.cpp's; every half links into one binary
// under the `parse` label, so the module's test command does not change.
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

// ECMA-262 13.3.5.1 evaluates `new NewExpression` with an EMPTY argument
// list, so this is the same tree `new Foo()` produces — deliberately
// indistinguishable, because the two constructs mean the same thing.
TEST_CASE("new without an argument list is the empty argument list") {
    CHECK(parseAndDump("const p = new Foo;") == parseAndDump("const p = new Foo();"));
    CHECK(parseAndDump("const p = new Foo;") ==
          "(module t\n"
          "  (const p\n"
          "    (new Foo\n"
          "    )\n"
          "  )\n"
          ")\n");
}

// The callee is a MemberExpression, so the whole `a.b.c` chain is what gets
// constructed. A parser that stopped the callee at `a` would produce
// `(member .c (member .b (new a)))` instead — a different tree, not a
// differently-printed one.
TEST_CASE("new takes the whole member chain as its callee") {
    CHECK(parseAndDump("const x = new a.b.c();") ==
          "(module t\n"
          "  (const x\n"
          "    (new\n"
          "      (member .c\n"
          "        (member .b\n"
          "          (ident a)\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

// The mirror of the case above, and the one that pins where the callee ENDS:
// the `(` closes the MemberExpression and supplies the Arguments, which makes
// `new a.b()` the construction and `.c` a read off its result.
TEST_CASE("an argument list ends the callee and the suffix chain resumes after it") {
    CHECK(parseAndDump("const x = new a.b().c;") ==
          "(module t\n"
          "  (const x\n"
          "    (member .c\n"
          "      (new\n"
          "        (member .b\n"
          "          (ident a)\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

// A parenthesized expression is a PrimaryExpression, so the call inside it
// belongs to the callee and the `()` after it is the `new`'s arguments. Not
// symmetrical with the case above, and the asymmetry is the point.
TEST_CASE("a parenthesized callee constructs the value the parentheses produce") {
    CHECK(parseAndDump("const x = new (getCtor())();") ==
          "(module t\n"
          "  (const x\n"
          "    (new\n"
          "      (call\n"
          "        (ident getCtor)\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("a computed member is a callee like any other") {
    CHECK(parseAndDump("const x = new Curves['Quad'](a);") ==
          "(module t\n"
          "  (const x\n"
          "    (new\n"
          "      (index\n"
          "        (ident Curves)\n"
          "        (string \"Quad\")\n"
          "      )\n"
          "      (ident a)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

// `new NewExpression`: the inner `new` takes the FIRST argument list and the
// outer one takes the second. A parser that ran the suffix chain on the inner
// `new` would build `(call (new F))` — construct once, then call the result.
TEST_CASE("new on a new constructs the inner result") {
    CHECK(parseAndDump("const x = new new F(1)(2);") ==
          "(module t\n"
          "  (const x\n"
          "    (new\n"
          "      (new F\n"
          "        (number 1)\n"
          "      )\n"
          "      (number 2)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("this is a callee base") {
    CHECK(parseAndDump("const x = new this.constructor();") ==
          "(module t\n"
          "  (const x\n"
          "    (new\n"
          "      (member .constructor\n"
          "        (this)\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

// ECMA-262 13.3 has no OptionalExpression under `new`: the short circuit
// would have to hand `new` a value to construct and there is none. Named
// rather than parsed as something else.
TEST_CASE("an optional chain may not be a new callee") {
    const auto out = parseAndDump("const x = new a?.b();");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("an optional chain may not be the callee of 'new'") != std::string::npos);
}

TEST_CASE("new.target is diagnosed by name") {
    const auto out = parseAndDump("function f() { return new.target; }");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("unsupported construct: new.target") != std::string::npos);
}

// `async m() {}` is identifier-then-identifier, the same token shape a class
// FIELD has, and the field diagnostic used to claim it. three.js contains
// zero class fields and five async methods, so the message was wrong every
// time it fired. `async` stays contextual: ECMA-262 15.8.1 makes it a
// modifier only when a ClassElementName follows on the same line.
TEST_CASE("an async class method is diagnosed as one, not as a field") {
    const auto out = parseAndDump("class L { async loadAsync(url) { return 1; } }");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("unsupported construct: async method in a class body") != std::string::npos);
    CHECK(out.find("class field") == std::string::npos);
}

TEST_CASE("a class member named async is still an ordinary method") {
    const auto out = parseAndDump("class L { async() { return 1; } }");
    CHECK(out.find("(method async") != std::string::npos);
}

TEST_CASE("a class field is still diagnosed as a field") {
    const auto out = parseAndDump("class L { x = 1; }");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("unsupported construct: class field") != std::string::npos);
}

// `new super.x()` reaches parsePrimary because docs/0025 made a `new` callee a
// full expression, and `super` had no arm there. "expected expression" points
// at the wrong thing: `super` is the expression, it is just not one 13.3.7
// permits outside a method's [[HomeObject]].
TEST_CASE("`super` outside its production is named, not `expected expression`") {
    const auto out = parseAndDump("class A { m() { return 1; } }\n"
                                  "class B extends A { m() { return new super.m(); } }");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("`super` is only supported as") != std::string::npos);
    CHECK(out.find("expected expression") == std::string::npos);
}
