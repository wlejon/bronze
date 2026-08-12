// Where a binding lives and who can reach it: the seam
// `src/lower/lower_scope.cpp` implements (docs/0007, docs/0016). Scopes that
// shadow and uncover, the module record a top-level function reads through,
// the environment an arrow reaches `this` and `arguments` through, and the
// synthetic parameters that carry them in.

#include <doctest/doctest.h>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

TEST_CASE("an ordinary function that uses `arguments` takes it as a parameter") {
    // The arguments object is built from the caller's REAL argument list, so
    // it arrives the way the rest array does — from the call wrapper, as a
    // synthetic leading parameter — and the function stops being a
    // direct-call target (docs/0027 decision 3).
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function count() { return arguments.length; }\n"
        "console.log(count(1, 2));\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
    const il::Function* count = nullptr;
    for (const auto& fn : optMod->functions) {
        if (fn.name == "count") count = &fn;
    }
    REQUIRE(count != nullptr);
    CHECK(count->needsArguments);
    // The source declares no parameters; the synthetic one is the object.
    CHECK(count->params.size() == 1);
    CHECK(count->firstSourceParam() == 1);
    // Not `call @count`: the direct path cannot see the argument count.
    CHECK(il::print(*optMod).find("call @count") == std::string::npos);
}

TEST_CASE("a parameter named `arguments` wins, and no object is built") {
    // 10.2.11: the arguments object exists only where the name is not already
    // bound. A synthetic binding here would be a redeclaration of the
    // parameter (docs/0027 decision 3).
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(arguments) { return arguments; }\n"
        "console.log(f(42));\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
    for (const auto& fn : optMod->functions) {
        if (fn.name == "f") CHECK_FALSE(fn.needsArguments);
    }
}

TEST_CASE("an arrow reaches `this` through the environment, not a parameter") {
    // Lexical `this` (docs/0012 decision 3) is capture, not an extra
    // argument: the enclosing function writes its own `this` into slot 0 of
    // its environment record, and the arrow reads it back from there. So
    // the arrow gets no `__this` parameter at all and cannot be rebound by
    // the call site.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function C() { this.v = 1; this.get = () => this.v; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("env.set %2, 0, 0, %0") != std::string::npos);
    const size_t arrow = printed.find("func __anon_fn_1(%0: dynamic)");
    REQUIRE(arrow != std::string::npos);
    CHECK(printed.find("env.get %0, 0, 0", arrow) != std::string::npos);
}

TEST_CASE("a module function's locals do not leak into the module top level") {
    // The top level is a function body like any other and starts from an
    // empty scope (docs/0016 decision 3). Before this, lowering carried the
    // LAST module function's bindings into `main`, and the two faces of that
    // are both checked here. This one is the dangerous face: the read
    // resolved to a binding whose SSA value id names an unrelated
    // instruction in `main`, so it compiled and printed a plausible number.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(p) { let secret = 42; return p + secret; }\n"
        "console.log(f(1));\n"
        "console.log(secret);\n",
        diags, buf);

    // The observable moved with docs/0027 decision 1 and the test's point did
    // not: `secret` must not RESOLVE here. A leak would give an `env.get` or a
    // read of an SSA value in `main`; what it gets instead is the
    // unresolved-name instruction and the warning that names it.
    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
    CHECK(diags.render(buf).find("unresolved name 'secret'") != std::string::npos);
    CHECK(il::print(*optMod).find("ref.error \"secret\"") != std::string::npos);
}

TEST_CASE("a top-level let may share a name with a module function's local") {
    // The other face: this is ordinary JS that did not compile at all,
    // because the leaked binding made the top-level declaration look like a
    // redeclaration in the same scope.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function g() { let acc = 7; return acc; }\n"
        "let acc = 'module';\n"
        "console.log(g());\n"
        "console.log(acc);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("a block declaration shadows an enclosing one and then uncovers it") {
    // Leaving a scope uncovers what its declarations hid; it does not delete
    // the name. `let x = 1; { let x = 2; } x` reported `undefined variable`.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f() {\n"
        "  let x = 1;\n"
        "  { let x = 10; console.log(x); }\n"
        "  return x;\n"
        "}\n"
        "console.log(f());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("a top-level function declaration reaches a module-level binding") {
    // docs/0016 decision 1. The module scope is a singleton, so its record is
    // published by `main` and loaded by the module functions that need it —
    // which is what lets them stay direct-call targets.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let count = 5;\n"
        "function read() { return count; }\n"
        "function bump() { count += 1; }\n"
        "bump();\n"
        "console.log(read());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("module.env.set") != std::string::npos);
    CHECK(text.find("module.env.get") != std::string::npos);
    // Still a direct call: the whole point of not desugaring these into
    // closures (docs/0016 decision 1).
    CHECK(text.find("call @read") != std::string::npos);
}

TEST_CASE("an update expression on a captured binding goes through the environment") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function outer() { let n = 0; return () => ++n; }\n"
        "console.log(outer()());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("env.get") != std::string::npos);
    CHECK(text.find("env.set") != std::string::npos);
}
