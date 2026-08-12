// Strict mode — `src/parse/parser_strict.cpp`: which code the Directive
// Prologue selects it for, and the early errors that only strict code has.
//
// Two kinds of assertion here, and they are two different questions. The
// PROLOGUE tests read the dump, because "is this code strict?" is a fact about
// the tree and the dump is where the tree is visible. The EARLY ERROR tests
// read the diagnostic text, because an early error produces no tree at all —
// and they check the message names the construct, which is the house rule that
// distinguishes a diagnosis from a refusal.

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
// it parses, because "the early error fired" is the whole assertion and an
// empty string would silently satisfy a `find` on it.
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

// Whether the SCRIPT is strict, asked of the dump's module head rather than of
// the whole text. A bare search for "strict" would be answered by the string
// literal the directive itself is — which is exactly what the negative tests
// below are about, so every one of them would pass for the wrong reason.
bool moduleIsStrict(const std::string& dump) { return contains(dump, "(module t strict"); }

}  // namespace

// ---- the Directive Prologue (ECMA-262 11.2.2) ------------------------------

TEST_CASE("a leading \"use strict\" selects strict mode for the script") {
    CHECK(moduleIsStrict(parseAndDump("\"use strict\";\nlet x = 1;\n")));
}

TEST_CASE("the directive is decided by the RAW text, so an escape disables it") {
    // 11.2.2: the literal must contain no escape sequences. Both of these
    // DENOTE "use strict" and neither is the directive, which is why the check
    // compares the characters between the quotes and not the decoded value.
    CHECK_FALSE(moduleIsStrict(parseAndDump("\"use\\u0020strict\";\n")));
    CHECK_FALSE(moduleIsStrict(parseAndDump("\"use \\163trict\";\n")));
    CHECK_FALSE(moduleIsStrict(parseAndDump("\"\\x75se strict\";\n")));
    // A line continuation is an escape too, and denotes nothing at all.
    CHECK_FALSE(moduleIsStrict(parseAndDump("\"use stri\\\nct\";\n")));
}

TEST_CASE("single quotes are a spelling and not an escape") {
    CHECK(moduleIsStrict(parseAndDump("'use strict';\n")));
}

TEST_CASE("the prologue is the whole leading run of string-literal statements") {
    CHECK(moduleIsStrict(parseAndDump("\"use asm\";\n\"use strict\";\n")));
    // …and it ENDS at the first statement that is not one.
    CHECK_FALSE(moduleIsStrict(parseAndDump("const x = 1;\n\"use strict\";\n")));
}

TEST_CASE("a string literal that continues an expression is not a statement") {
    // ASI supplies no semicolon before a `(` or a `[`, so the literal is a
    // callee or a base and the ExpressionStatement is longer than it. Reading
    // it as a directive would make these files strict on the strength of a
    // token that is not a statement at all.
    CHECK_FALSE(moduleIsStrict(parseAndDump("\"use strict\"\n[0];\n")));
    CHECK_FALSE(moduleIsStrict(parseAndDump("\"use strict\" + \"\";\n")));
    // With a newline and a token that cannot continue, ASI does supply one.
    CHECK(moduleIsStrict(parseAndDump("\"use strict\"\nlet x = 1;\n")));
}

TEST_CASE("a function's own prologue makes it strict inside sloppy code") {
    const auto out = parseAndDump("function f() {\n  \"use strict\";\n}\nfunction g() {}\n");
    CHECK(contains(out, "(function f () strict"));
    // …and the mode is given back: `g` is a sibling, not an heir.
    CHECK(contains(out, "(function g ()\n"));
    CHECK_FALSE(moduleIsStrict(out));
}

TEST_CASE("strictness is inherited by every function form written inside") {
    const auto out = parseAndDump(
        "\"use strict\";\n"
        "function f() {}\n"
        "const e = function () {};\n"
        "const a = () => 1;\n"
        "const o = { m() {}, get p() { return 1; } };\n");
    CHECK(contains(out, "(function f () strict"));
    CHECK(contains(out, "(function-expr <anon> () strict"));
    CHECK(contains(out, "(arrow-expr <anon> () strict"));
    // An object-literal method's IL symbol is `obj.<n>.<name>`: a method name
    // is a property key and never a binding, so it must not be spelled as one.
    CHECK(contains(out, "(function-expr obj.0.m () strict"));
    CHECK(contains(out, "(function-expr get p () strict"));
}

TEST_CASE("a class body is strict whether or not anything said so") {
    // 10.2.11 / 15.7. The file has no directive anywhere in it.
    const auto out = parseAndDump(
        "class C {\n"
        "  m() {}\n"
        "  static s() {}\n"
        "  get p() { return 1; }\n"
        "}\n"
        "function after() {}\n");
    CHECK(contains(out, "(function-expr C.m () strict"));
    CHECK(contains(out, "(function-expr C.s () strict"));
    CHECK(contains(out, "(function-expr C.get p () strict"));
    // The synthesized constructor is class code too.
    CHECK(contains(out, "(function-expr C.constructor () strict"));
    // And the mode does not leak past the closing brace.
    CHECK(contains(out, "(function after ()\n"));
}

// ---- the early errors ------------------------------------------------------

TEST_CASE("delete of an unqualified identifier is a strict early error") {
    // 13.5.1.1. The message names the identifier, so a reader is told which
    // `delete` in the file is meant.
    const auto errs = parseErrors("\"use strict\";\nlet x = 1;\ndelete x;\n");
    CHECK(contains(errs, "strict mode"));
    CHECK(contains(errs, "delete"));
    CHECK(contains(errs, "'x'"));
    // Parentheses do not help: the rule is stated over what they contain.
    CHECK(contains(parseErrors("\"use strict\";\nlet x = 1;\ndelete (x);\n"), "delete"));
    // A property delete is legal in both modes and must stay so.
    CHECK_FALSE(contains(parseAndDump("\"use strict\";\nconst o = {};\ndelete o.k;\n"), "ERRORS"));
}

TEST_CASE("delete of an identifier PARSES in sloppy code") {
    // bronze refuses it there too, but at LOWERING and for a different reason
    // (a binding is not a property). This is the early error's boundary: the
    // parser must hand the sloppy spelling on rather than diagnose it, or the
    // two refusals would be one and the message would cite the wrong rule.
    CHECK_FALSE(contains(parseAndDump("let x = 1;\ndelete x;\n"), "ERRORS"));
}

TEST_CASE("duplicate parameter names are a strict early error") {
    // 15.2.1. The check runs after the BODY, because a function's own
    // directive is what makes its parameter list subject to the rule.
    const auto errs = parseErrors("function f(a, a) {\n  \"use strict\";\n}\n");
    CHECK(contains(errs, "duplicate parameter name 'a'"));
    CHECK(contains(parseErrors("\"use strict\";\nfunction f(a, a) {}\n"), "duplicate parameter"));
    // A pattern binds names too, and they count.
    CHECK(contains(parseErrors("\"use strict\";\nfunction f({ a }, a) {}\n"),
                   "duplicate parameter"));
    // Sloppy code admits them.
    CHECK_FALSE(contains(parseAndDump("function f(a, a) {}\n"), "ERRORS"));
}

TEST_CASE("the with statement is diagnosed by name in both modes") {
    // 14.11.1 makes it a strict early error; bronze has not built its object
    // environment record in either mode, so the sloppy reading is a named
    // refusal rather than a call of a variable named `with`.
    CHECK(contains(parseErrors("\"use strict\";\nwith (o) { }\n"), "strict mode"));
    CHECK(contains(parseErrors("\"use strict\";\nwith (o) { }\n"), "'with'"));
    CHECK(contains(parseErrors("with (o) { }\n"), "'with'"));
}

TEST_CASE("legacy octal is refused in both modes, and names the literal") {
    // 12.9.3's LegacyOctalIntegerLiteral and 12.9.4.2's LegacyOctalEscapeSequence
    // are strict early errors. bronze refuses both in EVERY mode, because
    // reading `017` as decimal 17 or as octal 15 are both defensible and they
    // differ — which is the situation the house rule says to diagnose rather
    // than choose. These assertions pin that the refusal is not conditional.
    CHECK(contains(parseErrors("\"use strict\";\nconst n = 017;\n"), "legacy octal literal"));
    CHECK(contains(parseErrors("const n = 017;\n"), "legacy octal literal '017'"));
    CHECK(contains(parseErrors("\"use strict\";\nconst s = \"\\01\";\n"), "legacy octal escape"));
    CHECK(contains(parseErrors("const s = \"\\01\";\n"), "legacy octal escape"));
}

TEST_CASE("eval and arguments may not be bound or assigned in strict code") {
    // 13.15.1 for the targets, and the same names as binding identifiers.
    CHECK(contains(parseErrors("\"use strict\";\neval = 1;\n"), "'eval' may not be assigned"));
    CHECK(contains(parseErrors("\"use strict\";\narguments += 1;\n"),
                   "'arguments' may not be assigned"));
    CHECK(contains(parseErrors("\"use strict\";\nlet eval = 1;\n"), "may not be bound"));
    CHECK(contains(parseErrors("\"use strict\";\nfunction f(arguments) {}\n"), "may not be bound"));
    CHECK(contains(parseErrors("\"use strict\";\nfunction eval() {}\n"), "may not be bound"));
    CHECK(contains(parseErrors("\"use strict\";\ntry { } catch (eval) { }\n"), "may not be bound"));
    CHECK(contains(parseErrors("\"use strict\";\nlet x = 1;\n++eval;\n"), "may not be assigned"));
    // Reading them is legal; only binding and writing are not.
    CHECK_FALSE(contains(parseAndDump("\"use strict\";\nconst n = arguments;\n"), "ERRORS"));
    // Sloppy code admits all of it.
    CHECK_FALSE(contains(parseAndDump("let eval = 1;\neval = 2;\n"), "ERRORS"));
}

TEST_CASE("the nine future reserved words are refused in strict code only") {
    // 12.7.2. `let` is on the spec's list too, but bronze's lexer already makes
    // it a keyword token, so it can never reach the check as an identifier —
    // it is in the table for the reader, not for the parser.
    const char* const words[] = {"implements", "interface", "package", "private",
                                 "protected",  "public",    "static",  "yield"};
    for (const char* word : words) {
        const std::string decl = std::string("\"use strict\";\nlet ") + word + " = 1;\n";
        CHECK_MESSAGE(contains(parseErrors(decl), "reserved word"), word);
        const std::string ref = std::string("\"use strict\";\nconst n = ") + word + ";\n";
        CHECK_MESSAGE(contains(parseErrors(ref), "reserved word"), word);
        // Sloppy code admits every one of them as an ordinary name.
        const std::string sloppy = std::string("let ") + word + " = 1;\n";
        CHECK_MESSAGE(!contains(parseAndDump(sloppy), "ERRORS"), word);
    }
}

TEST_CASE("a function declaration inside a block is refused in strict code") {
    // ECMA-262 14.1 makes it block-scoped there; Annex B.3.3 gives sloppy code
    // the legacy hoisting bronze implements. bronze has not built the
    // block-scoped form, so in strict code the construct is named rather than
    // compiled with the other mode's scoping — a `typeof f` after the block
    // would otherwise answer "function" where the language says "undefined".
    CHECK(contains(parseErrors("\"use strict\";\nif (1) { function f() {} }\n"),
                   "a function declaration inside a block in strict code"));
    CHECK(contains(parseErrors("\"use strict\";\n{ function f() {} }\n"),
                   "function declaration inside a block"));
    CHECK(contains(parseErrors("class C { m() { if (1) { function f() {} } } }\n"),
                   "function declaration inside a block"));
    // Directly in a script or a function body is the position 14.1 admits.
    CHECK_FALSE(contains(parseAndDump("\"use strict\";\nfunction f() {}\n"), "ERRORS"));
    CHECK_FALSE(
        contains(parseAndDump("\"use strict\";\nfunction f() { function g() {} }\n"), "ERRORS"));
    // Sloppy code keeps the hoisting it always had.
    CHECK_FALSE(contains(parseAndDump("if (1) { function f() {} }\n"), "ERRORS"));
}

TEST_CASE("a reserved word is still a property name in strict code") {
    // 12.7.1: a reserved word is a name only after `.` or in a property-key
    // slot, and the strict list changes nothing about that — `o.static` and
    // `class C { static m() {} }` are both ordinary.
    CHECK_FALSE(contains(parseAndDump("\"use strict\";\nconst o = { static: 1 };\n"), "ERRORS"));
    CHECK_FALSE(contains(parseAndDump("\"use strict\";\nconst o = {};\nconst v = o.public;\n"),
                         "ERRORS"));
    CHECK_FALSE(contains(parseAndDump("class C { static m() {} }\n"), "ERRORS"));
}
