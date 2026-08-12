// The statements that move control somewhere other than the next one, and the
// expression that declines to: `switch`, labelled `break`/`continue`, `for-in`,
// and the optional chain. Grouped here rather than in lower_test.cpp because
// they share one question — what the IL edge out of this construct is, and
// which early errors keep an edge from being built that no `throw` could
// rescue.

#include <doctest/doctest.h>

#include "il/print.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"

using namespace bronze;

namespace {

std::unique_ptr<ast::Module> parseSource(std::string_view src, DiagnosticSink& diags,
                                         SourceBuffer& buf) {
    buf = SourceBuffer("test.ts", std::string(src));
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return nullptr;
    return Parser(std::move(tokens), diags).parseModule("test");
}

// The `--no-infer` path: an early error in lowering must not depend on
// anything inference proved, so every test here uses it.
std::optional<il::Module> lowerSource(std::string_view src, DiagnosticSink& diags,
                                      SourceBuffer& buf) {
    auto astMod = parseSource(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    return lower::lowerModule(*astMod, diags);
}

// Lowers `src` expecting failure and returns the rendered diagnostics.
std::string errorFor(std::string_view src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = lowerSource(src, diags, buf);
    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    return diags.render(buf);
}

std::string printOf(std::string_view src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = lowerSource(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    return il::print(*optMod);
}

}  // namespace

TEST_CASE("for-in snapshots the key list before walking it") {
    // The keys are materialized once, into an array, and the loop is then the
    // same iterator walk `for-of` uses. A `forin.keys` inside the loop body
    // would mean the snapshot is being retaken.
    const std::string printed = printOf("const o = { a: 1 };\nfor (const k in o) { console.log(k); }\n");
    CHECK(printed.find("forin.keys") != std::string::npos);
    CHECK(printed.find("iter.open") != std::string::npos);
}

TEST_CASE("a class method is defined non-enumerably, not assigned") {
    // ECMA-262 15.7.14 gives a method `enumerable: false`, and an assignment
    // cannot say that — so a `prop.set` here would silently put every method
    // into `Object.keys` and `for-in` on every instance.
    const std::string printed = printOf("class C { m() { return 1; } }\nconst c = new C();\n");
    CHECK(printed.find("method.def") != std::string::npos);
}

TEST_CASE("an optional link branches on nullish") {
    // The short circuit is a real edge, not a runtime check inside the property
    // helper: the whole point is that the rest of the chain is not executed.
    const std::string printed = printOf("const o = { a: 1 };\nconsole.log(o?.a);\n");
    CHECK(printed.find("is.nullish") != std::string::npos);
    CHECK(printed.find("br %") != std::string::npos);
}

TEST_CASE("a lexical declaration directly in a switch case belongs to the whole body") {
    // 14.12.2 makes the CaseBlock one declarative environment, so `x` is a
    // binding of the switch and not of the clause that spells it — it gets an
    // environment slot however few closures reach it, because the slot is
    // where the uninitialized marker lives.
    const std::string printed =
        printOf("switch (1) {\n  case 1:\n    console.log(x);\n    break;\n"
                "  case 2:\n    let x = 1;\n}\n");
    CHECK(printed.find("env.create") != std::string::npos);
    CHECK(printed.find("env.init.tdz") != std::string::npos);
    // The read in the clause ABOVE the declaration is the checked form: which
    // clause a jump entered is not a fact any position in the source holds.
    CHECK(printed.find("env.get.tdz") != std::string::npos);
    CHECK(printed.find("\"x\"") != std::string::npos);
}

TEST_CASE("a function declaration directly in a switch case is still named") {
    // The one form the CaseBlock's scope does not carry: 8.6.2 instantiates a
    // function declaration for the whole scope before any clause runs, which
    // would mean hoisting it out of the clause it is written in.
    const std::string rendered =
        errorFor("switch (1) {\n  case 1:\n    function f() {}\n}\n");
    CHECK(rendered.find("unsupported construct: a 'function' declaration directly in a switch "
                        "case") != std::string::npos);
    CHECK(rendered.find("wrap the case body in a block") != std::string::npos);
}

TEST_CASE("a block wrapping the case body accepts the same declaration") {
    // The counterpart to the error above: the diagnostic names a fix, and the
    // fix has to work, or it is a refusal wearing a suggestion.
    const std::string printed = printOf("switch (1) {\n  case 1: {\n    let x = 1;\n"
                                        "    console.log(x);\n  }\n}\n");
    CHECK(printed.find("strict.eq") != std::string::npos);
}

TEST_CASE("a duplicate label in scope is an early error") {
    const std::string rendered =
        errorFor("outer: for (let i = 0; i < 1; i++) {\n  outer: for (let j = 0; j < 1; j++) {}\n}\n");
    CHECK(rendered.find("duplicate label 'outer' (it is already in scope)") != std::string::npos);
}

TEST_CASE("two sibling statements may share a label") {
    // 14.13.1 checks the ENCLOSING label set, so reuse after the first label
    // is out of scope is legal — the test above must not be over-broad.
    const std::string printed = printOf("lp: for (let i = 0; i < 1; i++) { break lp; }\n"
                                        "lp: for (let i = 0; i < 1; i++) { break lp; }\n");
    CHECK(printed.find("jump b") != std::string::npos);
}

TEST_CASE("continue naming a labelled block is an early error") {
    const std::string rendered = errorFor("blk: {\n  continue blk;\n}\n");
    CHECK(rendered.find("continue label 'blk' does not name an enclosing loop") != std::string::npos);
}

TEST_CASE("a jump naming a label that is not in scope is an early error") {
    const std::string rendered = errorFor("for (let i = 0; i < 1; i++) { break nowhere; }\n");
    CHECK(rendered.find("break label 'nowhere' does not name an enclosing labelled statement") !=
          std::string::npos);
}

TEST_CASE("a label does not cross a function boundary") {
    // A label is not a value and does not close over anything, so a `break`
    // inside a nested function cannot name the enclosing function's label.
    const std::string rendered =
        errorFor("outer: for (let i = 0; i < 1; i++) {\n  const f = () => { break outer; };\n}\n");
    CHECK(rendered.find("does not name an enclosing labelled statement") != std::string::npos);
}

TEST_CASE("break and continue outside any target are named separately") {
    CHECK(errorFor("break;\n").find("break statement outside of a loop or switch") !=
          std::string::npos);
    CHECK(errorFor("continue;\n").find("continue statement outside of a loop") != std::string::npos);
    // A switch is breakable and is not iterable, so an unlabelled `continue`
    // inside one with no enclosing loop is still an error while `break` is
    // not (ECMA-262 14.12 vs 14.9).
    CHECK(errorFor("switch (1) { case 1: continue; }\n").find(
              "continue statement outside of a loop") != std::string::npos);
    const std::string printed = printOf("switch (1) { case 1: break; }\n");
    CHECK(printed.find("jump b") != std::string::npos);
}
