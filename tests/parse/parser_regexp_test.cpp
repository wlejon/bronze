// Regular expression LITERALS, split from parser_test.cpp along the seam
// parser_literal.cpp already names. What is pinned here is the two things the
// parser decides about one: that its body is taken verbatim rather than decoded
// like a string, and that its pattern is compiled where it is written, so a
// malformed regular expression is a compile error and not a surprise the first
// time the line runs.

// The doctest main is parser_test.cpp's; every half links into one binary
// under the `parse` label, so the module's test command does not change.
#include <doctest/doctest.h>

#include <string>

#include "ast/dump.h"
#include "lex/lexer.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

std::string parseRegExp(std::string_view src) {
    SourceBuffer buf("t.ts", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    auto mod = Parser(std::move(tokens), diags).parseModule("t");
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod != nullptr);
    return ast::dump(*mod);
}

}  // namespace

TEST_CASE("a regular expression literal keeps its pattern verbatim") {
    // `\d` is TWO characters here and one escape in a string literal, which is
    // the whole reason the pattern is not decoded: decoding it would leave the
    // matcher a `d` to match.
    const auto out = parseRegExp("const re = /\\d+\\n/gi;\n");
    CHECK(out.substr(0, 7) != "ERRORS:");
    CHECK(out.find("(regexp /\\d+\\n/gi)") != std::string::npos);
}

TEST_CASE("the flags are the token's tail and the last slash is the delimiter") {
    const auto out = parseRegExp("const re = /a\\/b/y;\n");
    CHECK(out.find("(regexp /a\\/b/y)") != std::string::npos);
    const auto none = parseRegExp("const re = /a/;\n");
    CHECK(none.find("(regexp /a/)") != std::string::npos);
}

TEST_CASE("a pattern that does not compile is a compile error where it is written") {
    for (const char* src : {"const re = /(/;\n", "const re = /a{2,1}/;\n",
                            "const re = /\\1/;\n", "const re = /a**/;\n"}) {
        const auto out = parseRegExp(src);
        CAPTURE(src);
        CHECK(out.substr(0, 7) == "ERRORS:");
        CHECK(out.find("invalid regular expression") != std::string::npos);
    }
    // An unclosed CLASS never reaches the pattern compiler: the `/` inside it
    // is an ordinary character, so the literal runs to the end of the line and
    // the LEXER is what refuses it.
    const auto unclosedClass = parseRegExp("const re = /[a/;\n");
    CHECK(unclosedClass.find("unterminated regular expression literal") != std::string::npos);
}

TEST_CASE("a construct bronze refuses is named at the literal, not at the run") {
    const auto lookbehind = parseRegExp("const re = /(?<=a)b/;\n");
    CHECK(lookbehind.find("lookbehind") != std::string::npos);
    const auto property = parseRegExp("const re = /\\p{L}/;\n");
    CHECK(property.find("unicode property escapes") != std::string::npos);
    const auto unicode = parseRegExp("const re = /a/u;\n");
    CHECK(unicode.find("`u` flag") != std::string::npos);
}
