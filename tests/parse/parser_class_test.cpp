// Private class elements and static blocks — `src/parse/parser_class.cpp`.
//
// ECMA-262 15.7.1 states its private-name rules as SYNTAX errors, and bronze's
// stance is hard errors over silent fallbacks, so every one of them is a
// compile error here rather than something a program can observe at run time.
// Two kinds of assertion, for the reason parser_strict_test.cpp gives: a rule
// that admits the code is checked against the DUMP, and one that refuses it is
// checked against the diagnostic text, which must name the construct.

// The doctest main is parser_test.cpp's; every file here links into one
// binary under the `parse` label.

#include <doctest/doctest.h>

#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

std::string parseAndDump(std::string_view src) {
    SourceBuffer buf("t.js", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = Parser(std::move(tokens), diags).parseModule("t");
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod != nullptr);
    return ast::dump(*mod);
}

// The rendered diagnostics of a source that must NOT parse. Fails the test if
// it parses, because "the early error fired" is the whole assertion.
std::string parseErrors(std::string_view src) {
    SourceBuffer buf("t.js", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return diags.render(buf);
    auto mod = Parser(std::move(tokens), diags).parseModule("t");
    REQUIRE_MESSAGE(diags.hasErrors(), "expected a diagnosed early error, got a parse");
    (void)mod;
    return diags.render(buf);
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a private name is one token and keeps its '#'") {
    const std::string dump = parseAndDump("class C { #x = 1; read() { return this.#x; } }");
    CHECK(contains(dump, "(field #x"));
    CHECK(contains(dump, "(member .#x"));
    // Not a property: the dump of a public field of the same spelling would be
    // `(field x`, and the two must not render alike.
    CHECK_FALSE(contains(dump, "(field x\n"));
}

TEST_CASE("every shape of private element parses") {
    const std::string dump = parseAndDump(
        "class C {\n"
        "  #f = 1;\n"
        "  #m() {}\n"
        "  get #p() { return 1; }\n"
        "  set #p(v) {}\n"
        "  static #s = 2;\n"
        "  static #sm() {}\n"
        "  *#g() {}\n"
        "  async #a() {}\n"
        "}\n");
    CHECK(contains(dump, "(field #f"));
    CHECK(contains(dump, "(method #m"));
    CHECK(contains(dump, "(get #p"));
    CHECK(contains(dump, "(set #p"));
    CHECK(contains(dump, "(static-field #s"));
    CHECK(contains(dump, "(static-method #sm"));
    CHECK(contains(dump, "(method #g"));
    CHECK(contains(dump, "(method #a"));
}

TEST_CASE("a static block is its own class element") {
    const std::string dump = parseAndDump("class C { static v = 1; static { this.w = 2; } }");
    CHECK(contains(dump, "(static-field v"));
    CHECK(contains(dump, "(static-block"));
    // `static` is still contextual: a member named `static` is not a modifier.
    const std::string named = parseAndDump("class D { static() { return 1; } }");
    CHECK(contains(named, "(method static"));
}

TEST_CASE("a private reference may stand above its declaration") {
    // 15.7.1 is stated over the whole ClassBody, so the parser cannot check a
    // reference against the declarations it has seen SO FAR.
    const std::string dump = parseAndDump("class C { read() { return this.#x; } #x = 1; }");
    CHECK(contains(dump, "(member .#x"));
}

TEST_CASE("a nested class reaches an outer private name and shadows a repeated one") {
    const std::string dump = parseAndDump(
        "class Outer {\n"
        "  #a = 1;\n"
        "  m() { class Inner { #b = 2; both(o) { return [o.#b, this.#a]; } } }\n"
        "}\n");
    CHECK(contains(dump, "(member .#b"));
    CHECK(contains(dump, "(member .#a"));
}

TEST_CASE("an undeclared private name is a syntax error") {
    const std::string errs = parseErrors("class C { m() { return this.#y; } }");
    CHECK(contains(errs, "private name '#y' is not declared by any enclosing class"));
}

TEST_CASE("an inner class's private name does not escape outward") {
    // `#b` is Inner's, so Outer's own method may not mention it — the private
    // environments nest, and resolution runs innermost OUT, never inward.
    const std::string errs = parseErrors(
        "class Outer {\n"
        "  m() { class Inner { #b = 1; } }\n"
        "  n(o) { return o.#b; }\n"
        "}\n");
    CHECK(contains(errs, "private name '#b' is not declared by any enclosing class"));
}

TEST_CASE("a private name outside a class body is a syntax error") {
    const std::string errs = parseErrors("const o = {}; o.#x;");
    CHECK(contains(errs, "private name '#x' outside a class body"));
    CHECK(contains(errs, "a private name may only be used inside the body of a class"));
}

TEST_CASE("`delete` of a private member is a syntax error") {
    const std::string errs = parseErrors("class C { #x = 1; m() { delete this.#x; } }");
    CHECK(contains(errs, "'delete' of the private member '#x' is a SyntaxError"));
    CHECK(contains(errs, "13.5.1.1"));
}

TEST_CASE("a duplicate private name is a syntax error") {
    SUBCASE("two fields") {
        const std::string errs = parseErrors("class C { #x = 1; #x = 2; }");
        CHECK(contains(errs, "duplicate private name '#x'"));
    }
    SUBCASE("a field and a method") {
        const std::string errs = parseErrors("class C { #x = 1; #x() {} }");
        CHECK(contains(errs, "duplicate private name '#x'"));
    }
    SUBCASE("two getters") {
        const std::string errs = parseErrors("class C { get #p() { return 1; } get #p() { return 2; } }");
        CHECK(contains(errs, "duplicate private name '#p'"));
    }
    SUBCASE("a static and an instance half of one accessor") {
        // 15.7.1 admits the pair only when both halves agree about `static`.
        const std::string errs = parseErrors("class C { get #p() { return 1; } static set #p(v) {} }");
        CHECK(contains(errs, "duplicate private name '#p'"));
    }
}

TEST_CASE("one getter and one setter of a name is not a duplicate") {
    const std::string dump = parseAndDump("class C { get #p() { return 1; } set #p(v) {} }");
    CHECK(contains(dump, "(get #p"));
    CHECK(contains(dump, "(set #p"));
}

TEST_CASE("'#constructor' is never a valid private name") {
    const std::string errs = parseErrors("class C { #constructor = 1; }");
    CHECK(contains(errs, "'#constructor' is not a valid private name"));
}

TEST_CASE("a private name outside `.` and `in` is a syntax error") {
    SUBCASE("as an expression of its own") {
        const std::string errs = parseErrors("class C { #x = 1; m() { return #x; } }");
        CHECK(contains(errs, "may only appear after '.' or on the left of 'in'"));
    }
    SUBCASE("as the right operand of `in`") {
        const std::string errs = parseErrors("class C { #x = 1; m(o) { return o in #x; } }");
        CHECK(contains(errs, "may only appear after '.' or on the left of 'in'"));
    }
}

TEST_CASE("`#x in o` parses as a relational expression") {
    const std::string dump = parseAndDump("class C { #x = 1; static has(o) { return #x in o; } }");
    CHECK(contains(dump, "(binary in"));
    CHECK(contains(dump, "(ident #x)"));
}

TEST_CASE("a lone '#' is still an unrecognized character") {
    // The PrivateName token is `#` plus an IdentifierName with nothing between
    // them; the `#` alone is not a token at all.
    const std::string errs = parseErrors("const a = 1 # 2;");
    CHECK(contains(errs, "unrecognized character '#'"));
}

TEST_CASE("a private member is a destructuring assignment target") {
    // 13.15.1 admits any simple assignment target, and `this.#x` is one. The
    // grammar is the whole of this test; that the store is a private-element
    // write rather than a property write — the thing that would otherwise put
    // an ordinary "#x" property on the object — is
    // tests/oracle/cases/class_private_destructuring.js.
    const char* const accepted[] = {
        "class C { #x; m(v) { ({ a: this.#x } = v); } }",
        "class C { #x; m(v) { [this.#x] = v; } }",
        "class C { #x; m(v) { ({ a: this.#x = 5 } = v); } }",
        "class C { #x; m(v) { [this.#x = 5] = v; } }",
        "class C { #x; m(v) { ({ a: { b: this.#x } } = v); } }",
        "class C { #x; m(v) { [...this.#x] = v; } }",
        "class C { #x; m(v) { ({ ...this.#x } = v); } }",
        "class C { static #s; static m(v) { ({ a: C.#s } = v); } }",
    };
    for (const char* src : accepted) {
        CHECK_MESSAGE(parseAndDump(src).substr(0, 7) != "ERRORS:", src);
    }
    const std::string dump = parseAndDump("class C { #x; m(v) { ({ a: this.#x } = v); } }");
    CHECK(contains(dump, "(elem a: <target>"));
    CHECK(contains(dump, "(member .#x"));
}

TEST_CASE("an undeclared private name is refused in a destructuring target too") {
    const std::string errs = parseErrors("class C { #x; m(v) { [this.#nope] = v; } }");
    CHECK(contains(errs, "private name '#nope' is not declared by any enclosing class"));
}

TEST_CASE("class extends MemberExpression and dynamic superclass expressions") {
    const std::string dump1 = parseAndDump("class EditorControls extends THREE.EventDispatcher {}");
    CHECK(contains(dump1, "(class EditorControls extends THREE.EventDispatcher"));
    CHECK(contains(dump1, "(super-call THREE.EventDispatcher"));

    const std::string dump2 = parseAndDump("class A extends x.y.Z {}");
    CHECK(contains(dump2, "(class A extends x.y.Z"));
    CHECK(contains(dump2, "(super-call x.y.Z"));

    const std::string dump3 = parseAndDump("class A extends (B) {}");
    CHECK(contains(dump3, "(class A"));
    CHECK(contains(dump3, "(super-call"));

    const std::string dump4 = parseAndDump("class A extends getBase() {}");
    CHECK(contains(dump4, "(class A"));
    CHECK(contains(dump4, "(super-call"));

    const std::string dump5 = parseAndDump(
        "class Derived extends THREE.Base {\n"
        "  constructor() { super(); }\n"
        "  m() { return super.m(); }\n"
        "}\n");
    CHECK(contains(dump5, "(class Derived extends THREE.Base"));
    CHECK(contains(dump5, "(super-call THREE.Base"));
    CHECK(contains(dump5, "(super-member THREE.Base.m)"));

    const std::string exprDump = parseAndDump("const C = class extends THREE.EventDispatcher {};");
    CHECK(contains(exprDump, "(class-expr <anon> extends THREE.EventDispatcher"));
}
