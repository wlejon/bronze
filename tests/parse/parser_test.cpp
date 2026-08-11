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
          // `export function f` records an export entry beside the
          // declaration, because that is the one shape every export form
          // reduces to — `export { a as b }` and `export ... from` cannot be
          // spelled as a flag on a declaration (docs/0023).
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

    // A class accessor is BUILT (docs/0019), so what is pinned here is that
    // it parses as one member with an accessor head rather than as a method
    // named `get`, and that the forms around it stay named.
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

TEST_CASE("ES2015 parameter and spread syntax parses into nodes of its own") {
    // Each form must reach a DISTINCT node, because each lowers differently:
    // a default is a branch, a rest is an array the convention builds, a
    // spread is a walk over a container, a pattern is a read per element
    // (docs/0017). Dumping any two of them alike would hide a wrong lowering.
    struct Case {
        const char* src;
        const char* expected;
    };
    const Case cases[] = {
        {"function f(a, b = 2) {}", "(param b"},
        {"const g = (a = 1) => a;", "(param a"},
        {"function f(...r) {}", "(function f (...r)"},
        {"class C { m(...r) {} }", "(function-expr C.m (...r)"},
        {"function h([a]) {}", "(pattern-array"},
        {"function h({a}) {}", "(pattern-object"},
        {"const [a, b] = [1, 2];", "(const <pattern>"},
        {"const { x } = { x: 1 };", "(pattern-object"},
        {"f(...[1]);", "(spread"},
        {"const c = [...[1]];", "(spread"},
        {"const o = { ...x };", "(prop-spread"},
        {"[a, b] = [b, a];", "(destructuring-assign"},
        {"({ x } = o);", "(destructuring-assign"},
    };
    for (const auto& c : cases) {
        const auto out = parseAndDump(c.src);
        CHECK(out.substr(0, 7) != "ERRORS:");
        CHECK(out.find(c.expected) != std::string::npos);
    }
}

TEST_CASE("a pattern nests, renames, defaults and rests, and dumps each apart") {
    const auto out = parseAndDump("function h([a, [b], ...c], { d, e: f = 1, ...g }) {}");
    CHECK(out ==
          "(module t\n"
          "  (function h (<pattern> <pattern>)\n"
          "    (param <pattern>\n"
          "      (pattern-array\n"
          "        (elem a\n"
          "        )\n"
          "        (elem <pattern>\n"
          "          (pattern-array\n"
          "            (elem b\n"
          "            )\n"
          "          )\n"
          "        )\n"
          "        (elem ...c\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "    (param <pattern>\n"
          "      (pattern-object\n"
          "        (elem d: d\n"
          "        )\n"
          "        (elem e: f\n"
          "          (default\n"
          "            (number 1)\n"
          "          )\n"
          "        )\n"
          "        (elem ...g\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("the syntax around patterns that ECMA-262 forbids is named") {
    // Everything the grammar rules out, plus the two constructs bronze
    // deliberately does not build (docs/0017 decision 8). A silent
    // acceptance here is a wrong answer, not a missing feature.
    struct Case {
        const char* src;
        const char* expected;
    };
    const Case cases[] = {
        {"function f(...r, a) {}", "a rest parameter must be the last parameter"},
        {"function f(...r = 1) {}", "a rest parameter may not have a default value"},
        {"function f(...[a]) {}", "a rest parameter must be a plain name"},
        {"const [a, ...r, b] = x;", "a rest element must be the last element of an array pattern"},
        {"const { ...r, a } = x;", "a rest property must be the last element of an object pattern"},
        {"const { ...[a] } = x;", "expected a name after '...' in an object pattern"},
        {"let [a];", "a destructuring declaration requires an initializer"},
        {"const [a, , b] = x;", "unsupported construct: an elision (a hole) in an array pattern"},
        {"const c = [1, , 2];", "unsupported construct: an elision (a hole) in an array literal"},
        {"[o.x] = y;",
         "unsupported construct: a destructuring assignment target that is not a name or a "
         "nested pattern"},
        {"[a] += b;", "a destructuring pattern may only be the target of '='"},
        {"const x = ...y;", "'...' is only allowed in an argument list"},
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

TEST_CASE("a cover-initialized name parses, and dumps as the pattern-only form it is") {
    // `{ x = 1 }` is legal ONLY as the left of a `=` (ECMA-262 13.2.5.1), and
    // the parser cannot know which it is until it reads on. So it parses, and
    // dumps under a head of its own — the error for the literal reading of it
    // belongs to lowering, which is the first pass that knows no `=` came.
    const auto cover = parseAndDump("const o = { x = 1 };");
    CHECK(cover.substr(0, 7) != "ERRORS:");
    CHECK(cover.find("(prop-cover-init x") != std::string::npos);

    const auto refined = parseAndDump("({ x = 1 } = o);");
    CHECK(refined.substr(0, 7) != "ERRORS:");
    CHECK(refined.find("(destructuring-assign") != std::string::npos);
    CHECK(refined.find("(default") != std::string::npos);
}

TEST_CASE("a try statement consumes its finally block") {
    // `finally` is a reserved word, and the parser has to consume the block
    // after it: left behind, it was read as an expression statement and the
    // construct lowering NAMES was reported as stray punctuation instead.
    const auto both = parseAndDump("try { f(); } catch (e) { g(); } finally { h(); }");
    CHECK(both.substr(0, 7) != "ERRORS:");
    CHECK(both.find("(try\n") != std::string::npos);
    CHECK(both.find("(catch e\n") != std::string::npos);
    CHECK(both.find("(finally\n") != std::string::npos);
    // All three parts keep their own statements: the body's call is a child of
    // the try and not of the catch.
    CHECK(both.find("(ident f)") < both.find("(catch e"));
    CHECK(both.find("(ident g)") < both.find("(finally"));
    CHECK(both.find("(ident h)") > both.find("(finally"));

    const auto onlyFinally = parseAndDump("try { f(); } finally { h(); }");
    CHECK(onlyFinally.substr(0, 7) != "ERRORS:");
    CHECK(onlyFinally.find("(catch") == std::string::npos);
    CHECK(onlyFinally.find("(finally\n") != std::string::npos);

    // A catch parameter is a BindingPattern (ECMA-262 14.15), and it is
    // optional: both forms dump under a head that says which one it is.
    const auto destructured = parseAndDump("try { f(); } catch ({ message }) { g(); }");
    CHECK(destructured.substr(0, 7) != "ERRORS:");
    CHECK(destructured.find("(catch <pattern>") != std::string::npos);
    CHECK(destructured.find("(pattern-object") != std::string::npos);

    const auto bindingless = parseAndDump("try { f(); } catch { g(); }");
    CHECK(bindingless.substr(0, 7) != "ERRORS:");
    CHECK(bindingless.find("(catch <none>") != std::string::npos);

    // `throw` carries its expression rather than dropping it.
    const auto thrown = parseAndDump("throw new Error('x');");
    CHECK(thrown.substr(0, 7) != "ERRORS:");
    CHECK(thrown.find("(throw") != std::string::npos);
    CHECK(thrown.find("(new") != std::string::npos);

    // A `try` with neither is a syntax error, not a silently accepted block.
    const auto bare = parseAndDump("try { f(); }");
    CHECK(bare.substr(0, 7) == "ERRORS:");
    CHECK(bare.find("a 'try' requires a 'catch' or a 'finally'") != std::string::npos);
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

TEST_CASE("a switch parses its clauses, and only one may be `default`") {
    const auto out = parseAndDump("switch (x) { case 1: a(); default: b(); case 2: c(); }\n");
    CHECK(out.find("(switch") != std::string::npos);
    CHECK(out.find("(case") != std::string::npos);
    // `default` in the MIDDLE is legal and keeps its position: ECMA-262
    // 14.12.4 walks the case list for a match and only then falls back to the
    // default clause, wherever it was written (docs/0018 decision 5).
    CHECK(out.find("(default") != std::string::npos);

    const auto two = parseAndDump("switch (x) { default: a(); default: b(); }\n");
    CHECK(two.substr(0, 7) == "ERRORS:");
    CHECK(two.find("a switch may have only one 'default' clause") != std::string::npos);

    const auto stray = parseAndDump("switch (x) { a(); }\n");
    CHECK(stray.substr(0, 7) == "ERRORS:");
    CHECK(stray.find("expected 'case' or 'default' in a switch body") != std::string::npos);
}

TEST_CASE("a label fronts exactly one statement, and not a declaration") {
    const auto out = parseAndDump("outer: while (x) { break outer; }\n");
    CHECK(out.find("(label outer") != std::string::npos);

    // ECMA-262 14.13: the LabelledItem is a Statement or a
    // FunctionDeclaration, so `let` is not one — and a label on a `let` reads
    // as if it scoped the binding, which it does not.
    const auto decl = parseAndDump("lbl: let x = 1;\n");
    CHECK(decl.substr(0, 7) == "ERRORS:");
    CHECK(decl.find("a label may not front a declaration") != std::string::npos);
}

TEST_CASE("an optional chain is not an assignment or update target") {
    // ECMA-262 13.3.9: an OptionalExpression is never a valid AssignmentTarget,
    // because there is no reference to write through when the chain
    // short-circuits. Both spellings are early errors rather than a write that
    // sometimes does nothing (docs/0018 decision 8).
    const auto assign = parseAndDump("a?.b = 1;\n");
    CHECK(assign.substr(0, 7) == "ERRORS:");
    CHECK(assign.find("an optional chain is not a valid assignment target") != std::string::npos);

    const auto inc = parseAndDump("a?.b++;\n");
    CHECK(inc.substr(0, 7) == "ERRORS:");
    CHECK(inc.find("an optional chain is not a valid target for '++' or '--'") !=
          std::string::npos);
}

TEST_CASE("`?.` before a digit is the conditional operator, not a chain") {
    // ECMA-262 12.8 gives `?.` a lookahead restriction: `a?.5:b` must lex as
    // `? .5 : b`, or the ternary with a fractional consequent stops parsing.
    const auto out = parseAndDump("const r = a ? .5 : 1;\nconst s = a?.5:1;\n");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(number 0.5)") != std::string::npos);
}

TEST_CASE("an object literal accessor is one property with two halves") {
    // The two halves of one name dump under separate heads, because that is
    // exactly what distinguishes them — `(prop x` twice would print `get x`
    // and `set x` identically (docs/0019 decision 4).
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

TEST_CASE("`delete` is a reference operator, and a binding is not a reference") {
    const auto prop = parseAndDump("delete o.a;\n");
    CHECK(prop.substr(0, 7) != "ERRORS:");
    CHECK(prop.find("(unary delete") != std::string::npos);

    // `delete x` is a SyntaxError in strict mode (ECMA-262 13.5.1.1), and the
    // message names the property form the author almost certainly meant.
    const auto binding = parseAndDump("const x = 1;\ndelete x;\n");
    CHECK(binding.substr(0, 7) != "ERRORS:");  // it parses; lowering refuses it
}
