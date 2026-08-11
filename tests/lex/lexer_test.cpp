#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <vector>

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

TEST_CASE("`...` lexes as one token, not three dots") {
    // Rest and spread are the only things it spells, and the parser can
    // only name them if it sees them as one token.
    DiagnosticSink diags;
    SourceBuffer buf("t.js", "f(...a);");
    const auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(tokens.size() > 3);
    CHECK(tokens[2].kind == TokenKind::Ellipsis);
    CHECK(tokens[2].text == "...");
    CHECK(tokens[3].kind == TokenKind::Identifier);
}

TEST_CASE("a single dot is still a dot") {
    DiagnosticSink diags;
    SourceBuffer buf("t.js", "a.b;");
    const auto tokens = Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(tokens.size() > 2);
    CHECK(tokens[1].kind == TokenKind::Dot);
}

TEST_CASE("a token records whether a line terminator precedes it") {
    // The only fact the lexer keeps about discarded trivia, and the only one
    // automatic semicolon insertion needs (docs/0014).
    auto lexed = lexAll("a b\nc");
    auto& tokens = lexed.tokens;
    REQUIRE(tokens.size() == 4);  // a, b, c, eof
    CHECK_FALSE(tokens[0].newlineBefore);  // nothing precedes the first token
    CHECK_FALSE(tokens[1].newlineBefore);  // `b`, same line
    CHECK(tokens[2].newlineBefore);        // `c`, next line
}

TEST_CASE("a comment reports the line terminator it spans or ends at") {
    // A line comment's terminating newline is left for the whitespace branch,
    // and a block comment spanning lines IS a line terminator for ASI
    // purposes (ECMA-262 12.4) — `return /*\n*/ 1` returns undefined.
    auto lineComment = lexAll("a // trailing\nb");
    CHECK(lineComment.tokens[1].newlineBefore);

    auto multiLineBlock = lexAll("a /* one\ntwo */ b");
    CHECK(multiLineBlock.tokens[1].newlineBefore);

    auto singleLineBlock = lexAll("a /* one */ b");
    CHECK_FALSE(singleLineBlock.tokens[1].newlineBefore);
}

TEST_CASE("end of input reports the line terminator before it") {
    // The last statement in a file that ends with a newline is terminated by
    // the end of input either way, but the restricted productions read this
    // flag on whatever token follows the keyword — including this one.
    auto withNewline = lexAll("return\n");
    CHECK(withNewline.tokens.back().kind == TokenKind::EndOfFile);
    CHECK(withNewline.tokens.back().newlineBefore);

    auto withoutNewline = lexAll("return");
    CHECK_FALSE(withoutNewline.tokens.back().newlineBefore);
}

TEST_CASE("the bitwise punctuation is longest-match, single form included") {
    // `&` and `|` used to exist only as the first half of `&&` and `||`, so
    // `a & b` reported an unrecognized character (docs/0015).
    auto lexed = lexAll("a & b && c | d || e ^ f ~g");
    auto& t = lexed.tokens;
    CHECK(t[1].kind == TokenKind::Amp);
    CHECK(t[3].kind == TokenKind::AmpAmp);
    CHECK(t[5].kind == TokenKind::Pipe);
    CHECK(t[7].kind == TokenKind::PipePipe);
    CHECK(t[9].kind == TokenKind::Caret);
    CHECK(t[11].kind == TokenKind::Tilde);
}

TEST_CASE("`>` fans out four ways and `>>>=` is the longest") {
    auto lexed = lexAll("a > b >= c >> d >>> e >>= f >>>= g << h <<= i");
    auto& t = lexed.tokens;
    CHECK(t[1].kind == TokenKind::Greater);
    CHECK(t[3].kind == TokenKind::GreaterEqual);
    CHECK(t[5].kind == TokenKind::GreaterGreater);
    CHECK(t[7].kind == TokenKind::GreaterGreaterGreater);
    CHECK(t[9].kind == TokenKind::GreaterGreaterAssign);
    CHECK(t[11].kind == TokenKind::GreaterGreaterGreaterAssign);
    CHECK(t[13].kind == TokenKind::LessLess);
    CHECK(t[15].kind == TokenKind::LessLessAssign);
}

TEST_CASE("`**` and `**=` do not lex as two stars") {
    auto lexed = lexAll("a ** b **= c * d");
    auto& t = lexed.tokens;
    CHECK(t[1].kind == TokenKind::StarStar);
    CHECK(t[3].kind == TokenKind::StarStarAssign);
    CHECK(t[5].kind == TokenKind::Star);
}

TEST_CASE("the operator keywords are keywords, not identifiers") {
    auto lexed = lexAll("typeof x; a instanceof B; \"k\" in o; void 0;");
    auto& t = lexed.tokens;
    CHECK(t[0].kind == TokenKind::KwTypeof);
    CHECK(t[4].kind == TokenKind::KwInstanceof);
    CHECK(t[8].kind == TokenKind::KwIn);
    CHECK(t[11].kind == TokenKind::KwVoid);
}

// A numeric literal is ONE token however it is spelled, so a malformed one
// can be diagnosed against its whole text (docs/0016 decision 5). The lexer
// deliberately does not decide what the digits mean.
TEST_CASE("radix prefixes, separators and exponents lex as one number token") {
    for (const char* src : {"0xFF", "0X10", "0o17", "0b1010", "0B1111_0000",
                            "1_000_000", "1e3", "1E3", "1.5e-3", "1e+7", ".5",
                            "1_0.2_5", "0xA_B"}) {
        auto lexed = lexAll(src);
        REQUIRE(lexed.tokens.size() == 2);  // the number, then eof
        CHECK(lexed.tokens[0].kind == TokenKind::NumberLiteral);
        CHECK(lexed.tokens[0].text == src);
    }
}

TEST_CASE("a malformed numeric literal is still one token") {
    // Handed over whole so the parser's message can quote it; splitting `0xFF`
    // into `0` and an identifier `xFF` is what it used to do, and that error
    // named nothing.
    for (const char* src : {"1_", "1__0", "0x_ff", "0b12", "0x"}) {
        auto lexed = lexAll(src);
        REQUIRE(lexed.tokens.size() == 2);
        CHECK(lexed.tokens[0].kind == TokenKind::NumberLiteral);
        CHECK(lexed.tokens[0].text == src);
    }
}

TEST_CASE("a dot after a number is a member access unless a digit follows") {
    auto member = lexAll("1.foo");
    REQUIRE(member.tokens.size() == 4);  // 1, ., foo, eof
    CHECK(member.tokens[0].kind == TokenKind::NumberLiteral);
    CHECK(member.tokens[0].text == "1");
    CHECK(member.tokens[1].kind == TokenKind::Dot);

    // `1e` has no exponent digits, so the `e` starts an identifier.
    auto notExponent = lexAll("1e");
    REQUIRE(notExponent.tokens.size() == 3);
    CHECK(notExponent.tokens[0].text == "1");
    CHECK(notExponent.tokens[1].kind == TokenKind::Identifier);

    // The leading-dot number lookahead must not swallow a spread.
    auto spread = lexAll("...x");
    REQUIRE(spread.tokens.size() == 3);
    CHECK(spread.tokens[0].kind == TokenKind::Ellipsis);
}

// ---- the `/` ambiguity (docs/0024 decision 1) --------------------------------
//
// `a / b` divides and `/ab/` is a literal, and only the PREVIOUS significant
// token tells them apart. Every case below is one a wrong answer would
// mis-parse silently rather than diagnose, which is why they are pinned here
// rather than left to the parser to notice.

static std::vector<TokenKind> kindsOf(const Lexed& lexed) {
    std::vector<TokenKind> kinds;
    for (const Token& t : lexed.tokens) kinds.push_back(t.kind);
    return kinds;
}

TEST_CASE("a slash after something that ends an expression is a division") {
    for (const char* src : {"a / b", "1 / b", "'s' / b", "(a) / b", "a[0] / b", "a++ / b",
                            "a-- / b", "this / b", "true / b", "null / b", "undefined / b",
                            "`t` / b"}) {
        auto lexed = lexAll(src);
        const auto kinds = kindsOf(lexed);
        CAPTURE(src);
        CHECK(std::find(kinds.begin(), kinds.end(), TokenKind::Slash) != kinds.end());
        CHECK(std::find(kinds.begin(), kinds.end(), TokenKind::RegExpLiteral) == kinds.end());
    }
}

TEST_CASE("a slash anywhere an expression may start is a regular expression") {
    for (const char* src : {"/ab/", "x = /ab/", "return /ab/", "typeof /ab/", "f(/ab/)",
                            "[/ab/]", "a === /ab/", "case /ab/:", "in /ab/", "new /ab/",
                            "delete /ab/", "void /ab/", "a, /ab/", "!/ab/", "a ? /ab/ : b",
                            "} /ab/"}) {
        auto lexed = lexAll(src);
        const auto kinds = kindsOf(lexed);
        CAPTURE(src);
        CHECK(std::find(kinds.begin(), kinds.end(), TokenKind::RegExpLiteral) != kinds.end());
    }
}

TEST_CASE("a regular expression literal is one token, delimiters and flags included") {
    auto lexed = lexAll("x = /ab+c/gi;");
    REQUIRE(lexed.tokens.size() == 5);  // x, =, /ab+c/gi, ;, eof
    CHECK(lexed.tokens[2].kind == TokenKind::RegExpLiteral);
    CHECK(lexed.tokens[2].text == "/ab+c/gi");
}

TEST_CASE("a slash inside a class or behind a backslash does not end the literal") {
    auto klass = lexAll("x = /[/]+/g");
    CHECK(klass.tokens[2].text == "/[/]+/g");
    auto escaped = lexAll(R"(x = /a\/b/)");
    CHECK(escaped.tokens[2].text == R"(/a\/b/)");
    // A `//` that follows an operand is still a comment, because the slash
    // ambiguity is decided before the literal is ever entered.
    auto comment = lexAll("a // /not a regexp/\nb");
    REQUIRE(comment.tokens.size() == 3);
    CHECK(comment.tokens[0].kind == TokenKind::Identifier);
    CHECK(comment.tokens[1].kind == TokenKind::Identifier);
}

TEST_CASE("an unterminated regular expression literal is diagnosed, never guessed") {
    lexAll("x = /abc", /*expectErrors=*/true);
    // A line terminator ends the literal: 12.9.5's RegularExpressionChar
    // excludes LineTerminator, so this is an error rather than a literal
    // running to the end of the file.
    lexAll("x = /abc\ny", /*expectErrors=*/true);
}
