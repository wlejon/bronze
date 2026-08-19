// Statements — `src/parse/parser_stmt.cpp`'s half: the productions that do
// not map to exactly one node (a BindingList is several declarations, the
// empty statement is none), the selection and jump forms, and the try
// statement's three separate scopes.

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

TEST_CASE("a switch parses its clauses, and only one may be `default`") {
    const auto out = parseAndDump("switch (x) { case 1: a(); default: b(); case 2: c(); }\n");
    CHECK(out.find("(switch") != std::string::npos);
    CHECK(out.find("(case") != std::string::npos);
    // `default` in the MIDDLE is legal and keeps its position: ECMA-262 14.12.4
    // walks the case list for a match and only then falls back to the default
    // clause, wherever it was written.
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

TEST_CASE("contextual keyword 'of' in statements and declarations") {
    const auto letOf = parseAndDump("let of = 1; const of = 2; var of = 3;\n");
    CHECK(letOf.substr(0, 7) != "ERRORS:");
    CHECK(letOf.find("(let of\n") != std::string::npos);
    CHECK(letOf.find("(const of\n") != std::string::npos);
    CHECK(letOf.find("(var of\n") != std::string::npos);

    const auto fnOf = parseAndDump("function of(of) { return of; }\n");
    CHECK(fnOf.substr(0, 7) != "ERRORS:");
    CHECK(fnOf.find("(function of (of)\n") != std::string::npos);

    const auto classOf = parseAndDump("class of {}\n");
    CHECK(classOf.substr(0, 7) != "ERRORS:");
    CHECK(classOf.find("(class of") != std::string::npos);

    const auto forOf = parseAndDump("for (const x of y) { f(x); }\n");
    CHECK(forOf.substr(0, 7) != "ERRORS:");
    CHECK(forOf.find("(for-of x\n") != std::string::npos);

    const auto forOfOf = parseAndDump("for (const of of y) { f(of); }\n");
    CHECK(forOfOf.substr(0, 7) != "ERRORS:");
    CHECK(forOfOf.find("(for-of of\n") != std::string::npos);

    const auto forClassic = parseAndDump("for (let of = 0; of < 10; of++) {}\n");
    CHECK(forClassic.substr(0, 7) != "ERRORS:");
    CHECK(forClassic.find("(for\n") != std::string::npos);
    CHECK(forClassic.find("(let of\n") != std::string::npos);

    const auto labeledOf = parseAndDump("of: while (true) { break of; continue of; }\n");
    CHECK(labeledOf.substr(0, 7) != "ERRORS:");
    CHECK(labeledOf.find("(label of\n") != std::string::npos);
    CHECK(labeledOf.find("(break of)") != std::string::npos);
    CHECK(labeledOf.find("(continue of)") != std::string::npos);
}
