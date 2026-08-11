#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>

#include "lex/lexer.h"

using namespace bronze;

// Tokens hold string_views into the SourceBuffer, so the buffer must outlive
// them — the fixture owns both (buffer behind a unique_ptr: moving a small
// std::string would invalidate SSO-backed views).
struct Lexed {
    std::unique_ptr<SourceBuffer> buf;
    std::vector<Token> tokens;
};

static Lexed lexAll(std::string_view src, bool expectErrors = false) {
    Lexed result;
    result.buf = std::make_unique<SourceBuffer>("t.ts", std::string(src));
    DiagnosticSink diags;
    result.tokens = Lexer(*result.buf, diags).lex();
    CHECK(diags.hasErrors() == expectErrors);
    return result;
}

TEST_CASE("keywords vs identifiers") {
    auto lexed = lexAll("const constant = 1;");
    auto& tokens = lexed.tokens;
    REQUIRE(tokens.size() == 6);  // const, constant, =, 1, ;, eof
    CHECK(tokens[0].kind == TokenKind::KwConst);
    CHECK(tokens[1].kind == TokenKind::Identifier);
    CHECK(tokens[1].text == "constant");
    CHECK(tokens[2].kind == TokenKind::Assign);
    CHECK(tokens[3].kind == TokenKind::NumberLiteral);
    CHECK(tokens[4].kind == TokenKind::Semicolon);
    CHECK(tokens[5].kind == TokenKind::EndOfFile);
}

TEST_CASE("operators including multi-char") {
    auto lexed = lexAll("a === b != c => d");
    auto& tokens = lexed.tokens;
    CHECK(tokens[1].kind == TokenKind::EqualEqualEqual);
    CHECK(tokens[3].kind == TokenKind::BangEqual);
    CHECK(tokens[5].kind == TokenKind::Arrow);
}

TEST_CASE("comments and strings") {
    auto lexed = lexAll("// line\n'str' /* block */ \"two\"");
    auto& tokens = lexed.tokens;
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].kind == TokenKind::StringLiteral);
    CHECK(tokens[0].text == "'str'");
    CHECK(tokens[1].text == "\"two\"");
}

TEST_CASE("new keyword vs identifiers") {
    auto lexed = lexAll("new Foo(newish);");
    auto& tokens = lexed.tokens;
    REQUIRE(tokens.size() == 7);  // new, Foo, (, newish, ), ;, eof
    CHECK(tokens[0].kind == TokenKind::KwNew);
    CHECK(tokens[0].text == "new");
    CHECK(tokens[1].kind == TokenKind::Identifier);
    CHECK(tokens[1].text == "Foo");
    CHECK(tokens[3].kind == TokenKind::Identifier);
    CHECK(tokens[3].text == "newish");
}

TEST_CASE("numbers with fraction") {
    auto lexed = lexAll("1.5 2");
    auto& tokens = lexed.tokens;
    CHECK(tokens[0].text == "1.5");
    CHECK(tokens[1].text == "2");
}

TEST_CASE("unrecognized input is a hard error") {
    (void)lexAll("let x = #", /*expectErrors=*/true);
}

TEST_CASE("unterminated string is a hard error") {
    (void)lexAll("'abc", /*expectErrors=*/true);
}

TEST_CASE("a template literal lexes as a sequence, not one token") {
    // The interior of a substitution is ordinary source, lexed by the
    // ordinary loop. What the lexer has to get right is which `}` ends the
    // substitution: an object literal inside one opens a brace that must
    // not be mistaken for it.
    auto lexed = lexAll("`a${ {x: 1}.x }b`");
    auto& tokens = lexed.tokens;
    REQUIRE(tokens.size() > 2);
    CHECK(tokens[0].kind == TokenKind::TemplateHead);
    CHECK(tokens[tokens.size() - 2].kind == TokenKind::TemplateTail);

    auto whole = lexAll("`no subs`");
    CHECK(whole.tokens[0].kind == TokenKind::TemplateWhole);

    auto nested = lexAll("`a${`b${1}c`}d`");
    CHECK(nested.tokens[0].kind == TokenKind::TemplateHead);
    CHECK(nested.tokens[1].kind == TokenKind::TemplateHead);
    CHECK(nested.tokens[nested.tokens.size() - 2].kind == TokenKind::TemplateTail);

    lexAll("`unterminated", /*expectErrors=*/true);
}
