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

TEST_CASE("automatic semicolon insertion supplies the terminators ECMA-262 does") {
    // Insertion happens at a token on a later line, at a `}`, and at the end
    // of input — and nowhere else (docs/0014).
    const auto newline = parseAndDump("let a = 1\nlet b = 2\n");
    CHECK(newline.substr(0, 7) != "ERRORS:");
    CHECK(newline.find("(let a") != std::string::npos);
    CHECK(newline.find("(let b") != std::string::npos);

    const auto brace = parseAndDump("function f() { return 1 }");
    CHECK(brace.substr(0, 7) != "ERRORS:");

    const auto eof = parseAndDump("const c = 1");
    CHECK(eof.substr(0, 7) != "ERRORS:");
}

TEST_CASE("a missing semicolon on one line is still an error") {
    // The rule is about the offending token, not about semicolons being
    // optional: without a line break there is nothing to insert at.
    const auto out = parseAndDump("let a = 1 let b = 2;");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("expected ';' after declaration") != std::string::npos);
}

TEST_CASE("a line break does not break an expression that continues") {
    // `const c = 1 \n + 2` is one addition: parseExpr consumes the `+ 2`
    // before anything asks for a semicolon, which is why ASI cannot live in
    // the lexer.
    const auto out = parseAndDump("const c = 1\n+ 2;");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(binary +") != std::string::npos);
}

TEST_CASE("the restricted productions end at a line terminator") {
    // `return` on its own line returns undefined; the expression below it is
    // a separate statement, not the returned value.
    const auto ret = parseAndDump("function f() {\n  return\n  1;\n}");
    CHECK(ret.substr(0, 7) != "ERRORS:");
    // The `1` is an expression STATEMENT, so it dumps under its own head
    // rather than as the return's operand — `return 1;` has no `(expr`.
    CHECK(ret.find("(expr") != std::string::npos);
    CHECK(ret.find("(number 1)") != std::string::npos);

    // The identifier on the next line is the next statement, not a label.
    const auto brk = parseAndDump("while (a) {\n  break\n  b;\n}");
    CHECK(brk.substr(0, 7) != "ERRORS:");
    CHECK(brk.find("(break)") != std::string::npos);
    CHECK(brk.find("(ident b)") != std::string::npos);

    // Postfix `++` after a line break is the NEXT statement's prefix `++`.
    const auto inc = parseAndDump("let e = d\n++d;");
    CHECK(inc.substr(0, 7) != "ERRORS:");
    CHECK(inc.find("(unary ++pre") != std::string::npos);
    CHECK(inc.find("(unary ++post") == std::string::npos);

    // `throw` is the one restricted production with nothing to fall back to.
    const auto thr = parseAndDump("throw\n  1;");
    CHECK(thr.substr(0, 7) == "ERRORS:");
    CHECK(thr.find("a line terminator is not allowed between 'throw'") != std::string::npos);
}

TEST_CASE("the semicolons in a `for` header are punctuation, not terminators") {
    // ASI must not apply to them: they belong to the production, so a header
    // missing one is an error even across a line break.
    const auto out = parseAndDump("for (let i = 0\n i < 3; i++) {}");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("expected ';' after for init") != std::string::npos);
}

TEST_CASE("`import` is diagnosed by name rather than as a missing expression") {
    const auto out = parseAndDump("import x from \"y\";");
    CHECK(out.substr(0, 7) == "ERRORS:");
    CHECK(out.find("unsupported construct: import declaration") != std::string::npos);
}

TEST_CASE("the precedence ladder groups the new operators the way ECMA-262 does") {
    // One expression per rung, each written so a wrong grouping produces a
    // visibly different tree (docs/0015 decision 6).
    const auto shiftBindsTighterThanRelational = parseAndDump("const a = 1 << 2 < 8;");
    CHECK(shiftBindsTighterThanRelational.find(
              "(binary <\n"
              "      (binary <<") != std::string::npos);

    // `&` above equality, `^` above `&`, `|` above `^`: `a | b ^ c & d` is
    // `a | (b ^ (c & d))`.
    const auto bitwise = parseAndDump("const a = 1 | 2 ^ 3 & 4;");
    CHECK(bitwise.find(
              "(binary |\n"
              "      (number 1)\n"
              "      (binary ^\n"
              "        (number 2)\n"
              "        (binary &\n") != std::string::npos);

    // Equality binds tighter than `&`, which is the classic surprise:
    // `1 & 2 == 2` is `1 & (2 == 2)`.
    const auto equality = parseAndDump("const a = 1 & 2 == 2;");
    CHECK(equality.find(
              "(binary &\n"
              "      (number 1)\n"
              "      (binary ==") != std::string::npos);

    // `in` and `instanceof` are relational, so additive is below them.
    const auto relational = parseAndDump("const a = 1 + 1 in o;");
    CHECK(relational.find(
              "(binary in\n"
              "      (binary +") != std::string::npos);
}

TEST_CASE("`**` is right-associative and refuses an unparenthesized unary left operand") {
    // `2 ** 3 ** 2` is 512, not 64.
    const auto right = parseAndDump("const a = 2 ** 3 ** 2;");
    CHECK(right.find(
              "(binary **\n"
              "      (number 2)\n"
              "      (binary **") != std::string::npos);

    // ECMA-262 declines to pick a reading for `-2 ** 2`, and so does bronze.
    const auto ambiguous = parseAndDump("const a = -2 ** 2;");
    CHECK(ambiguous.substr(0, 7) == "ERRORS:");
    CHECK(ambiguous.find("'**' cannot have an unparenthesized unary operand") !=
          std::string::npos);

    // Parenthesizing either way is accepted; the flag is about the source
    // form, not the node kind.
    CHECK(parseAndDump("const a = (-2) ** 2;").substr(0, 7) != "ERRORS:");
    CHECK(parseAndDump("const a = -(2 ** 2);").substr(0, 7) != "ERRORS:");
}

TEST_CASE("`??` cannot be mixed with `&&` or `||` without parentheses") {
    CHECK(parseAndDump("const a = b ?? c || d;").find(
              "'??' cannot be mixed with '&&' or '||'") != std::string::npos);
    CHECK(parseAndDump("const a = b || c ?? d;").find(
              "'??' cannot be mixed with '&&' or '||'") != std::string::npos);
    CHECK(parseAndDump("const a = b && c ?? d;").find(
              "'??' cannot be mixed with '&&' or '||'") != std::string::npos);
    CHECK(parseAndDump("const a = (b ?? c) || d;").substr(0, 7) != "ERRORS:");
}

TEST_CASE("assignment is right-associative and sits above the conditional") {
    // Both of these parsed the other way round before docs/0015 decision 8:
    // `x = cond ? a : b` assigned the CONDITION.
    const auto ternary = parseAndDump("x = c ? 1 : 2;");
    CHECK(ternary.find(
              "(binary =\n"
              "      (ident x)\n"
              "      (ternary") != std::string::npos);

    const auto chained = parseAndDump("a = b = 3;");
    CHECK(chained.find(
              "(binary =\n"
              "      (ident a)\n"
              "      (binary =\n") != std::string::npos);
}

TEST_CASE("a comma in an argument list is a separator, not the comma operator") {
    // The hazard in adding the lowest rung: wiring comma in at the wrong
    // level turns `f(a, b)` into a one-argument call.
    const auto call = parseAndDump("f(a, b);");
    CHECK(call.find("(ident a)") != std::string::npos);
    CHECK(call.find("(ident b)") != std::string::npos);
    CHECK(call.find("(binary ,") == std::string::npos);

    const auto array = parseAndDump("const xs = [a, b];");
    CHECK(array.find("(binary ,") == std::string::npos);

    // Parenthesized, it IS the operator.
    const auto real = parseAndDump("f((a, b));");
    CHECK(real.find("(binary ,") != std::string::npos);
}

// ---- declarations, the empty statement, literals (docs/0016) --------------

TEST_CASE("a BindingList is several declarations, not one and not a block") {
    // Each declarator is its own binding (ECMA-262 14.3.1), so they appear
    // side by side in the enclosing list. A BlockStmt here would give them a
    // scope of their own and hide them from the next statement.
    const auto out = parseAndDump("let a = 1, b = 2, c;\n");
    CHECK(out ==
          "(module t\n"
          "  (let a\n"
          "    (number 1)\n"
          "  )\n"
          "  (let b\n"
          "    (number 2)\n"
          "  )\n"
          "  (let c\n"
          "  )\n"
          ")\n");
}

TEST_CASE("a for header takes a BindingList too") {
    const auto out = parseAndDump("for (let i = 0, j = 4; i < j; i++) { }\n");
    CHECK(out.find("(let i\n") != std::string::npos);
    CHECK(out.find("(let j\n") != std::string::npos);
    CHECK(out.substr(0, 6) != "ERRORS");
}

TEST_CASE("the empty statement contributes no node") {
    const auto out = parseAndDump(";;\nlet x = 1;;\n;\n");
    CHECK(out ==
          "(module t\n"
          "  (let x\n"
          "    (number 1)\n"
          "  )\n"
          ")\n");
}

TEST_CASE("an empty statement is a legal loop body") {
    const auto out = parseAndDump("while (f()) ;\n");
    CHECK(out ==
          "(module t\n"
          "  (while\n"
          "    (call\n"
          "      (ident f)\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("object shorthand is the key and the identifier, and computed keys dump apart") {
    // Two constructs that lower differently must not dump identically
    // (docs/0012 decision 3): a written key is a constant, a computed one is
    // an expression evaluated before its value.
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

TEST_CASE("object method shorthand and a cover-initialized name are named errors") {
    const auto method = parseAndDump("const o = { m() { return 1; } };");
    CHECK(method.substr(0, 7) == "ERRORS:");
    CHECK(method.find("unsupported construct: object literal method shorthand") !=
          std::string::npos);

    const auto cover = parseAndDump("const o = { x = 1 };");
    CHECK(cover.substr(0, 7) == "ERRORS:");
    CHECK(cover.find("unsupported construct: destructuring assignment pattern") !=
          std::string::npos);
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
