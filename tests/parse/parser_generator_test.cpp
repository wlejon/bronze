// Generators: the `yield` nodes the parser builds. Named for
// `src/parse/parser_generator.cpp`, which is where every assertion here is
// decided.
//
// Whether a generator also RUNS correctly is tests/oracle/cases/generator_*,
// because a tree assertion passes just as happily when the parser is
// consistently wrong. What belongs here is the SHAPE the rest of the compiler
// is handed: a `yield` reaches lowering as a `yield`, exactly where it was
// written. A suspension is a brass coroutine suspend, which keeps every live
// intermediate in the frame, so no position of a `yield` needs rewriting.

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

TEST_CASE("a generator body keeps its control flow, and its yields") {
    // Every one of these was once refused by name. A `yield` is an expression
    // wherever an expression is legal (15.5.1), and the statement around it is
    // an ordinary statement: what the parser produces is the body as written,
    // with the yields still in it.
    const auto inLoop = parseAndDump("class C { *g() { for (;;) { yield 1; } } }");
    CHECK(inLoop.substr(0, 7) != "ERRORS:");
    CHECK(inLoop.find("(for") != std::string::npos);
    CHECK(inLoop.find("(yield") != std::string::npos);

    // The two loops that walk a container are ordinary statements too: the
    // iteration record they step is a value like any other, live across the
    // suspension in the coroutine frame.
    const auto inForOf = parseAndDump("class C { *g() { for (const v of xs) { yield v; } } }");
    CHECK(inForOf.substr(0, 7) != "ERRORS:");
    CHECK(inForOf.find("(for-of") != std::string::npos);
    CHECK(inForOf.find("(yield") != std::string::npos);

    const auto inForIn = parseAndDump("class C { *g() { for (const k in o) { yield k; } } }");
    CHECK(inForIn.substr(0, 7) != "ERRORS:");
    CHECK(inForIn.find("(for-in") != std::string::npos);
    CHECK(inForIn.find("(yield") != std::string::npos);

    // Both suspension forms in one such body.
    const auto bothInForOf =
        parseAndDump("class C { *g() { for (const v of xs) { yield v; yield* other(); } } }");
    CHECK(bothInForOf.substr(0, 7) != "ERRORS:");
    CHECK(bothInForOf.find("(yield*") != std::string::npos);

    const auto inIf = parseAndDump("class C { *g() { if (a) yield 1; } }");
    CHECK(inIf.substr(0, 7) != "ERRORS:");
    CHECK(inIf.find("(if") != std::string::npos);
    CHECK(inIf.find("(yield") != std::string::npos);

    const auto inSwitch = parseAndDump("class C { *g() { switch (a) { case 1: yield 1; } } }");
    CHECK(inSwitch.substr(0, 7) != "ERRORS:");
    CHECK(inSwitch.find("(switch") != std::string::npos);
    CHECK(inSwitch.find("(yield") != std::string::npos);

    const auto inTry = parseAndDump("class C { *g() { try { yield 1; } catch (e) {} } }");
    CHECK(inTry.substr(0, 7) != "ERRORS:");
    CHECK(inTry.find("(try") != std::string::npos);
    CHECK(inTry.find("(yield") != std::string::npos);

    const auto inBlock = parseAndDump("class C { *g() { { yield 1; } } }");
    CHECK(inBlock.substr(0, 7) != "ERRORS:");
    CHECK(inBlock.find("(yield") != std::string::npos);

    // A declaration at the top level of a generator body is a declaration.
    const auto declares = parseAndDump("class C { *g() { let a = 1; yield a; } }");
    CHECK(declares.substr(0, 7) != "ERRORS:");
    CHECK(declares.find("(let a") != std::string::npos);
    CHECK(declares.find("(yield") != std::string::npos);

    // `return` in a generator body is a `return`: 27.5.1.2 makes its argument
    // the `value` of the final result, both with an operand and without.
    const auto returnsValue = parseAndDump("class C { *g() { yield 1; return 2; } }");
    CHECK(returnsValue.substr(0, 7) != "ERRORS:");
    CHECK(returnsValue.find("(return") != std::string::npos);

    const auto returnsBare = parseAndDump("class C { *g() { yield 1; return; } }");
    CHECK(returnsBare.substr(0, 7) != "ERRORS:");
    CHECK(returnsBare.find("(return") != std::string::npos);

    const auto returnNested = parseAndDump("class C { *g() { if (a) return; yield 1; } }");
    CHECK(returnNested.substr(0, 7) != "ERRORS:");
    CHECK(returnNested.find("(return") != std::string::npos);

    // A `function*` declaration and a generator function expression carry the
    // same flag as a generator method, and dump under their own heads.
    const auto decl = parseAndDump("function* g() { yield 1; }");
    CHECK(decl.substr(0, 7) != "ERRORS:");
    CHECK(decl.find("(generator g") != std::string::npos);
    const auto expr = parseAndDump("const g = function* () { yield 1; };");
    CHECK(expr.substr(0, 7) != "ERRORS:");
    CHECK(expr.find("(generator-expr") != std::string::npos);
    const auto method = parseAndDump("class C { *each() { yield 1; } }");
    CHECK(method.find("(generator-expr C.each") != std::string::npos);

    // An ordinary function still dumps as one, `yield` or no `yield` nearby.
    CHECK(parseAndDump("function g() { return 1; }").find("(function g") != std::string::npos);
}

TEST_CASE("a yield stays inside the expression it was written in") {
    // The grammar allows a `yield` wherever an AssignmentExpression goes, and
    // the tree keeps it there: no temporaries, no hoisted pre-statements, no
    // `if` synthesized around a short-circuit arm. The suspension is a brass
    // coroutine suspend, so every intermediate live across it is in the frame.
    const char* positions[] = {
        "class C { *g() { const x = yield 1; } }",
        "class C { *g() { const x = (yield 1) + (yield 2); } }",
        "class C { *g() { f(yield 1); } }",
        "class C { *g() { const x = a && (yield 1); } }",
        "class C { *g() { const x = a ? (yield 1) : 2; } }",
        "class C { *g() { while (yield 1) { f(); } } }",
        "class C { *g() { do { f(); } while (yield 1); } }",
        "class C { *g() { for (let i = 0; i < 2; i += yield 1) {} } }",
        "class C { *g() { switch (a) { case yield 1: break; } } }",
        "class C { *g() { const x = o?.[yield 1]; } }",
        "class C { *g() { const x = delete (yield 1); } }",
        "class C { *g() { try { f(); } finally { yield 1; } } }",
        "class C { *g() { try { f(); } finally { yield* other(); } } }",
    };
    for (const char* src : positions) {
        CAPTURE(src);
        const auto dump = parseAndDump(src);
        CHECK(dump.substr(0, 7) != "ERRORS:");
        CHECK(dump.find("(yield") != std::string::npos);
        CHECK(dump.find("gen.") == std::string::npos);
    }

    const auto objectLiteral = parseAndDump("const o = { *g() { yield 1; } };");
    CHECK(objectLiteral.substr(0, 7) != "ERRORS:");
    CHECK(objectLiteral.find("(generator-expr") != std::string::npos);
}

TEST_CASE("`yield*` reaches the AST as a delegating yield") {
    // Delegation is a protocol (27.5.3.7) rather than a second operator, and
    // all of the protocol is built in lowering. What the parser owes it is one
    // node that says WHICH form was written, because the two lower to entirely
    // different machines and nothing downstream can recover the difference from
    // the operand.
    const auto delegating = parseAndDump("class C { *g() { yield* other(); } }");
    CHECK(delegating.substr(0, 7) != "ERRORS:");
    CHECK(delegating.find("(yield*") != std::string::npos);
    CHECK(delegating.find("(call") != std::string::npos);

    // And a plain `yield` is still a plain `yield`: the dump distinguishes them
    // because a tree that printed both as `(yield` would let the flag be
    // dropped without a single assertion noticing.
    const auto plain = parseAndDump("class C { *g() { yield other(); } }");
    CHECK(plain.substr(0, 7) != "ERRORS:");
    CHECK(plain.find("(yield*") == std::string::npos);

    // 15.5.1 has no production for a bare `yield*`: the delegating form takes
    // an AssignmentExpression, and a delegation with nothing to delegate to has
    // no iterator to open.
    const auto bare = parseAndDump("class C { *g() { yield*; } }");
    CHECK(bare.substr(0, 7) == "ERRORS:");

    // `yield [no LineTerminator here] *`, so a line break ends a bare `yield`
    // and the `*` after it starts a statement that no production admits.
    const auto broken = parseAndDump("class C { *g() { yield\n* other(); } }");
    CHECK(broken.substr(0, 7) == "ERRORS:");

    // In expression position it stays where it was written.
    const auto inExpr = parseAndDump("class C { *g() { const x = 1 + (yield* a); } }");
    CHECK(inExpr.substr(0, 7) != "ERRORS:");
    CHECK(inExpr.find("(yield*") != std::string::npos);
}

TEST_CASE("`yield` is contextual, and does not cross a function boundary") {
    // `yield` is contextual: outside a generator it is an ordinary name, and
    // a function written INSIDE one is not itself a generator.
    const auto ordinaryName = parseAndDump("function f() { const yield = 1; return yield; }");
    CHECK(ordinaryName.substr(0, 7) != "ERRORS:");
    const auto nestedFn = parseAndDump("class C { *g() { yield [1].map(function (v) { return v; }); } }");
    CHECK(nestedFn.substr(0, 7) != "ERRORS:");
    // The nested function is an ordinary one: `return v;` is not a generator
    // `return`.
    CHECK(nestedFn.find("(function-expr") != std::string::npos);
}
