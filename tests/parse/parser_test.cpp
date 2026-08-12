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
          // `export function f` records an export entry beside the declaration,
          // because that is the one shape every export form reduces to —
          // `export { a as b }` and `export... from` cannot be spelled as a
          // flag on a declaration.
          "  (export\n"
          "    (name max as max)\n"
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

TEST_CASE("const without initializer is a hard error") {
    const auto out = parseAndDump("const x;");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("initializer") != std::string::npos);
}

TEST_CASE("trailing garbage is a hard error, never dropped") {
    const auto out = parseAndDump("let x = 1; )");
    CHECK(out.substr(0, 7) == "ERRORS:");
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

TEST_CASE("a derived class with no constructor forwards every argument") {
    // The implicit `constructor(...args) { super(...args); }` of ECMA-262
    // 15.7.14. Rest and spread are what make it exact: the parent sees the
    // arguments the caller gave, all of them and no padding.
    const auto out = parseAndDump("class D extends B {}");
    CHECK(out ==
          "(module t\n"
          "  (class D extends B\n"
          "    (method constructor\n"
          "      (function-expr D.constructor (...args)\n"
          "        (expr\n"
          "          (super-call B\n"
          "            (spread\n"
          "              (ident args)\n"
          "            )\n"
          "          )\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("class members bronze has not built are named, not mis-parsed") {
    const auto field = parseAndDump("class C { x = 1; }");
    CHECK(field.find("unsupported construct: class field") != std::string::npos);

    // A class accessor is BUILT, so what is pinned here is that it parses as
    // one member with an accessor head rather than as a method named `get`, and
    // that the forms around it stay named.
    const auto getter = parseAndDump("class C { get x() { return 1; } }");
    CHECK(getter.substr(0, 7) != "ERRORS:");
    CHECK(getter.find("(get x") != std::string::npos);

    const auto setter = parseAndDump("class C { static set x(v) { this.q = v; } }");
    CHECK(setter.substr(0, 7) != "ERRORS:");
    CHECK(setter.find("(static-set x") != std::string::npos);

    // `get` is contextual in a class body too: a method may be called `get`.
    const auto namedGet = parseAndDump("class C { get() { return 1; } }");
    CHECK(namedGet.substr(0, 7) != "ERRORS:");
    CHECK(namedGet.find("(method get") != std::string::npos);

    const auto computedAccessor = parseAndDump("class C { get [k]() { return 1; } }");
    CHECK(computedAccessor.find("unsupported construct: a computed getter name") !=
          std::string::npos);

    // ECMA-262 15.4.1 fixes the arity of each half; both are early errors.
    const auto getterArity = parseAndDump("class C { get x(a) { return a; } }");
    CHECK(getterArity.find("a getter must take exactly no parameters") != std::string::npos);
    const auto setterArity = parseAndDump("class C { set x() {} }");
    CHECK(setterArity.find("a setter must take exactly one parameter") != std::string::npos);

    const auto computed = parseAndDump("class C { [k]() { return 1; } }");
    CHECK(computed.find("unsupported construct: computed method name") != std::string::npos);

    // A generator method is BUILT for the straight-line subset, so what was
    // pinned here as a refusal is now pinned as the desugaring: an ordinary
    // method whose body returns an iterator object over a step index. No
    // `yield` node reaches the AST.
    const auto gen = parseAndDump("class C { *each() { yield 1; } }");
    CHECK(gen.substr(0, 7) != "ERRORS:");
    CHECK(gen.find("(method each") != std::string::npos);
    CHECK(gen.find("(let gen.0.step") != std::string::npos);
    CHECK(gen.find("(arrow-expr gen.0.next") != std::string::npos);
    CHECK(gen.find("(prop @@iterator") != std::string::npos);

    // `*[Symbol.iterator]()` is the one computed member name bronze reads, and
    // it names the string the protocol uses.
    const auto symbolIter = parseAndDump("class C { *[Symbol.iterator]() { yield this.x; } }");
    CHECK(symbolIter.substr(0, 7) != "ERRORS:");
    CHECK(symbolIter.find("(method @@iterator") != std::string::npos);

    // The same key without the `*`: an iterator written out by hand.
    const auto handWritten = parseAndDump("class C { [Symbol.iterator]() { return this.it; } }");
    CHECK(handWritten.substr(0, 7) != "ERRORS:");
    CHECK(handWritten.find("(method @@iterator") != std::string::npos);

    const auto otherComputedGen = parseAndDump("class C { *[k]() { yield 1; } }");
    CHECK(otherComputedGen.find("unsupported construct: a computed generator name") !=
          std::string::npos);
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

TEST_CASE("a module is never returned with input left unconsumed") {
    // The predecessor's .form parser stopped at the first construct it could
    // not continue past and returned what it had, so files silently lost
    // everything after module 1. The rule here is that `parseModule` has two
    // outcomes and no third: a module that consumed every token, or a
    // diagnostic. Never a shorter module and silence.
    //
    // `parseStatement` diagnoses each of these before the end-of-input check
    // in `parseModule` can be reached, which is why that check has no case of
    // its own — it is the backstop for a production that returns without
    // consuming, and this test is what would notice one appearing.
    for (const std::string_view src : {
             "let a = 1; }",
             "let a = 1; )",
             "let a = 1; ]",
             "function f() {} }",
             "const o = { a: 1 }; case 2:",
             "let a = 1; let",
         }) {
        CAPTURE(src);
        SourceBuffer buf("t.ts", std::string(src));
        DiagnosticSink diags;
        auto tokens = Lexer(buf, diags).lex();
        auto mod = Parser(std::move(tokens), diags).parseModule("t");
        // Loud, and empty-handed: a partial module with no complaint is the
        // failure this pins against.
        CHECK(diags.hasErrors());
        CHECK(mod == nullptr);
    }
}
