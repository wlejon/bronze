// Generators: the straight-line subset bronze desugars, and the constructs
// outside it. Named for `src/parse/parser_generator.cpp`, which is where every
// assertion here is decided.
//
// Whether the desugaring also RUNS correctly is tests/oracle/cases/generator_*,
// because a tree assertion passes just as happily when the parser is
// consistently wrong. What belongs here is the refusals: each construct
// outside the subset has its OWN message, and someone who hits one has to be
// able to learn from it what bronze does support.

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

TEST_CASE("a generator outside the straight-line subset is refused by name") {
    // Each construct gets its OWN message: someone who hits one has to be able
    // to learn from it what bronze does support.
    const auto inLoop = parseAndDump("class C { *g() { for (;;) { yield 1; } } }");
    CHECK(inLoop.find("a `yield` inside a loop") != std::string::npos);

    const auto inIf = parseAndDump("class C { *g() { if (a) yield 1; } }");
    CHECK(inIf.find("a `yield` inside an `if`") != std::string::npos);

    const auto inSwitch = parseAndDump("class C { *g() { switch (a) { case 1: yield 1; } } }");
    CHECK(inSwitch.find("a `yield` inside a `switch`") != std::string::npos);

    const auto inTry = parseAndDump("class C { *g() { try { yield 1; } catch (e) {} } }");
    CHECK(inTry.find("a `yield` inside a `try`") != std::string::npos);

    const auto inBlock = parseAndDump("class C { *g() { { yield 1; } } }");
    CHECK(inBlock.find("a `yield` inside a nested block") != std::string::npos);

    const auto delegating = parseAndDump("class C { *g() { yield* other(); } }");
    CHECK(delegating.find("`yield*` (delegation)") != std::string::npos);

    // The value of a `yield` is what the generator is RESUMED with, which
    // arrives on the next call and which an index switch cannot deliver.
    const auto valueUsed = parseAndDump("class C { *g() { const x = yield 1; } }");
    CHECK(valueUsed.find("a `yield` inside an expression whose value is used") !=
          std::string::npos);

    const auto returnsValue = parseAndDump("class C { *g() { yield 1; return 2; } }");
    CHECK(returnsValue.find("`return <expr>;` in a generator") != std::string::npos);

    const auto returnsBare = parseAndDump("class C { *g() { yield 1; return; } }");
    CHECK(returnsBare.find("`return;` in a generator") != std::string::npos);

    const auto returnNested = parseAndDump("class C { *g() { if (a) return; yield 1; } }");
    CHECK(returnNested.find("`return` inside a generator body") != std::string::npos);

    const auto declares = parseAndDump("class C { *g() { let a = 1; yield a; } }");
    CHECK(declares.find("a declaration at the top level of a generator body") != std::string::npos);

    const auto objectLiteral = parseAndDump("const o = { *g() { yield 1; } };");
    CHECK(objectLiteral.find("a generator method in an object literal") != std::string::npos);

    // `yield` is contextual: outside a generator it is an ordinary name, and
    // a function written INSIDE one is not itself a generator.
    const auto ordinaryName = parseAndDump("function f() { const yield = 1; return yield; }");
    CHECK(ordinaryName.substr(0, 7) != "ERRORS:");
    const auto nestedFn = parseAndDump("class C { *g() { yield [1].map(function (v) { return v; }); } }");
    CHECK(nestedFn.substr(0, 7) != "ERRORS:");
}
