// Generators: the `yield` nodes the parser builds, the rewrite it runs over a
// generator body before handing it on, and the constructs still outside what
// bronze implements. Named for `src/parse/parser_generator.cpp` and
// `src/ast/yield_lift.cpp`, which is where every assertion here is decided.
//
// Whether the state machine also RUNS correctly is tests/oracle/cases/generator_*,
// because a tree assertion passes just as happily when the parser is
// consistently wrong. What belongs here is the SHAPE the rest of the compiler
// is handed: a `yield` reaches lowering as a `yield`, every one of them sits at
// a statement boundary, and each construct bronze cannot lift out of has its
// OWN message, so someone who hits one can learn from it what bronze supports.

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

TEST_CASE("every yield is lifted to a statement boundary before lowering sees it") {
    // `src/ast/yield_lift.cpp` normalizes a generator body so that no `yield`
    // is left inside a larger expression. This is not a syntax rule — the
    // grammar allows all of it — it is what makes a resume edge into the middle
    // of the body possible at all: an intermediate with no name cannot be
    // carried across a suspension, and after the rewrite every intermediate has
    // one. The temporaries are named `gen.<n>.t<k>`, per generator in the file.

    // The value of a `yield` is what the generator was RESUMED with, so it must
    // survive as a binding rather than as an expression value.
    const auto valueUsed = parseAndDump("class C { *g() { const x = yield 1; } }");
    CHECK(valueUsed.substr(0, 7) != "ERRORS:");
    CHECK(valueUsed.find("(let gen.0.t0") != std::string::npos);
    CHECK(valueUsed.find("(const x") != std::string::npos);

    // Both operands of a binary expression: the left one has to be pinned too,
    // because the `yield` on the right runs after it and before the addition.
    const auto binary = parseAndDump("class C { *g() { const x = (yield 1) + (yield 2); } }");
    CHECK(binary.substr(0, 7) != "ERRORS:");
    CHECK(binary.find("(let gen.0.t0") != std::string::npos);
    CHECK(binary.find("(let gen.0.t1") != std::string::npos);

    // A `yield` in an argument: the call happens after the suspension, so the
    // argument list is evaluated into temporaries first.
    const auto argument = parseAndDump("class C { *g() { f(yield 1); } }");
    CHECK(argument.substr(0, 7) != "ERRORS:");
    CHECK(argument.find("(let gen.0.t") != std::string::npos);

    // 13.13.1: the right operand of `&&` runs only if the left is truthy, so a
    // `yield` there becomes an `if`, not an unconditional pre-statement.
    const auto shortCircuit = parseAndDump("class C { *g() { const x = a && (yield 1); } }");
    CHECK(shortCircuit.substr(0, 7) != "ERRORS:");
    CHECK(shortCircuit.find("(if") != std::string::npos);
    CHECK(shortCircuit.find("(yield") != std::string::npos);

    // Same rule for the two arms of a conditional.
    const auto ternary = parseAndDump("class C { *g() { const x = a ? (yield 1) : 2; } }");
    CHECK(ternary.substr(0, 7) != "ERRORS:");
    CHECK(ternary.find("(if") != std::string::npos);

    // 14.7.4.9 re-tests the condition on every iteration, so a `yield` in a
    // `while` head becomes `while (true)` with the test inside the body.
    const auto whileHead = parseAndDump("class C { *g() { while (yield 1) { f(); } } }");
    CHECK(whileHead.substr(0, 7) != "ERRORS:");
    CHECK(whileHead.find("(break") != std::string::npos);

    // A generator with no `yield` at all is untouched: no temporaries appear.
    const auto plain = parseAndDump("class C { *g() { const x = a + b; return x; } }");
    CHECK(plain.substr(0, 7) != "ERRORS:");
    CHECK(plain.find("gen.0.t") == std::string::npos);

    // The prefix counts generators, so two in one file cannot collide.
    const auto two = parseAndDump(
        "class C { *g() { const x = yield 1; } *h() { const y = yield 2; } }");
    CHECK(two.find("gen.0.t0") != std::string::npos);
    CHECK(two.find("gen.1.t0") != std::string::npos);
}

TEST_CASE("a generator outside what bronze implements is refused by name") {
    // Each construct gets its OWN message: someone who hits one has to be able
    // to learn from it what bronze does support.

    // Delegation is a second walk suspended inside the outer one — a whole
    // protocol (27.5.3.7), not a second entry in a state table.
    const auto delegating = parseAndDump("class C { *g() { yield* other(); } }");
    CHECK(delegating.find("`yield*` (delegation") != std::string::npos);

    const auto objectLiteral = parseAndDump("const o = { *g() { yield 1; } };");
    CHECK(objectLiteral.find("a generator method in an object literal") != std::string::npos);

    // The lifter refuses what it cannot give a name to, and says which position.
    const auto inFinally = parseAndDump("class C { *g() { try { f(); } finally { yield 1; } } }");
    CHECK(inFinally.find("inside a `finally` block") != std::string::npos);

    const auto forOfBody = parseAndDump("class C { *g() { for (const v of xs) { yield v; } } }");
    CHECK(forOfBody.find("inside the body of a `for-of`") != std::string::npos);

    const auto forInBody = parseAndDump("class C { *g() { for (const k in o) { yield k; } } }");
    CHECK(forInBody.find("inside the body of a `for-in`") != std::string::npos);

    const auto caseTest = parseAndDump("class C { *g() { switch (a) { case yield 1: break; } } }");
    CHECK(caseTest.find("in the test of a `case` clause") != std::string::npos);

    const auto doWhile = parseAndDump("class C { *g() { do { f(); } while (yield 1); } }");
    CHECK(doWhile.find("unsupported construct: a `yield`") != std::string::npos);

    const auto update = parseAndDump("class C { *g() { for (let i = 0; i < 2; i += yield 1) {} } }");
    CHECK(update.find("unsupported construct: a `yield`") != std::string::npos);

    const auto optional = parseAndDump("class C { *g() { const x = o?.[yield 1]; } }");
    CHECK(optional.find("unsupported construct: a `yield`") != std::string::npos);

    const auto increment = parseAndDump("class C { *g() { const x = delete (yield 1); } }");
    CHECK(increment.find("unsupported construct: a `yield`") != std::string::npos);
}

TEST_CASE("`yield` is contextual, and does not cross a function boundary") {
    // `yield` is contextual: outside a generator it is an ordinary name, and
    // a function written INSIDE one is not itself a generator.
    const auto ordinaryName = parseAndDump("function f() { const yield = 1; return yield; }");
    CHECK(ordinaryName.substr(0, 7) != "ERRORS:");
    const auto nestedFn = parseAndDump("class C { *g() { yield [1].map(function (v) { return v; }); } }");
    CHECK(nestedFn.substr(0, 7) != "ERRORS:");
    // The nested function is an ordinary one, and the lifter does not reach
    // into it: `return v;` is not a generator `return`.
    CHECK(nestedFn.find("(function-expr") != std::string::npos);
}
