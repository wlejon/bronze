// Automatic semicolon insertion — the rule that decides where a statement ends
// when the program did not say. The restricted productions are the reason it
// cannot be "insert one wherever the parse would otherwise fail": `return`
// followed by a line terminator returns undefined, and the value on the next
// line becomes dead code.

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

TEST_CASE("automatic semicolon insertion supplies the terminators ECMA-262 does") {
    // Insertion happens at a token on a later line, at a `}`, and at the end of
    // input — and nowhere else.
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
