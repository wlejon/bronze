// Where a binding lives and who can reach it: the seam
// `src/lower/lower_scope.cpp` implements. Scopes that shadow and uncover, the
// module record a top-level function reads through, the environment an arrow
// reaches `this` and `arguments` through, and the synthetic parameters that
// carry them in.

#include <doctest/doctest.h>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

TEST_CASE("an ordinary function that uses `arguments` takes it as a parameter") {
    // The arguments object is built from the caller's REAL argument list, so it
    // arrives the way the rest array does — from the call wrapper, as a
    // synthetic leading parameter — and the function stops being a direct-call
    // target.
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
    // parameter.
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
    // Lexical `this` is capture, not an extra argument: the enclosing function
    // writes its own `this` into slot 0 of its environment record, and the
    // arrow reads it back from there. So the arrow gets no `__this` parameter
    // at all and cannot be rebound by the call site.
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
    // The top level is a function body like any other and starts from an empty
    // scope. Before this, lowering carried the LAST module function's bindings
    // into `main`, and the two faces of that are both checked here. This one is
    // the dangerous face: the read resolved to a binding whose SSA value id
    // names an unrelated instruction in `main`, so it compiled and printed a
    // plausible number.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(p) { let secret = 42; return p + secret; }\n"
        "console.log(f(1));\n"
        "console.log(secret);\n",
        diags, buf);

    // The observable moved when an unresolvable name became a runtime error,
    // and the test's point did not: `secret` must not RESOLVE here. A leak
    // would give an `env.get` or a read of an SSA value in `main`; what it gets
    // instead is the unresolved-name instruction and the warning that names it.
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
    // The module scope is a singleton, so its record is published by `main` and
    // loaded by the module functions that need it — which is what lets them
    // stay direct-call targets.
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
    // closures.
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

// --- the temporal dead zone ------------------------------------------------
//
// Where the uninitialized marker lives and which reads are checked. The
// BEHAVIOUR these produce is the oracle suite's (cases/temporal_dead_zone*);
// what is pinned here is the shape, because the cost of the mechanism is
// exactly "which bindings stopped living in SSA" and a silent widening of that
// set is how the first attempt put every local of every function in a record.

TEST_CASE("a lexical binding read above its declaration moves into the environment") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "{\n  try { console.log(v); } catch (e) { console.log(e.name); }\n"
        "  let v = 1;\n  console.log(v);\n}\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    // The slot, the marker in it, and the checked read that names the binding.
    CHECK(text.find("env.init.tdz") != std::string::npos);
    CHECK(text.find("env.get.tdz") != std::string::npos);
    CHECK(text.find("\"v\"") != std::string::npos);
}

TEST_CASE("a straight line of lexical declarations stays in SSA") {
    // The regression this exists for: an exposure scan that counted a
    // declaration's own name as a mention reported every `const` in a function
    // as read before itself, and moved the lot into an environment record —
    // which is a whole-program slowdown wearing a correctness fix's costume.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function calc(x) {\n  const doubled = x * 2;\n  const less = doubled - 1;\n"
        "  return less / 2;\n}\n"
        "console.log(calc(4));\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("env.init.tdz") == std::string::npos);
    CHECK(text.find("env.get.tdz") == std::string::npos);
    CHECK(text.find("env.create") == std::string::npos);
}

TEST_CASE("a hoisted function declaration is never given a dead zone") {
    // 8.6.2 instantiates a function declaration for its whole scope, so it is
    // initialized from the moment the scope is entered. Marking it lexical
    // would make calling it above where it is written a ReferenceError, which
    // is ordinary JavaScript that must keep working.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "{\n  console.log(hoisted());\n  function hoisted() { return 1; }\n"
        "  const seen = hoisted;\n  console.log(seen());\n}\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("env.get.tdz") == std::string::npos);
    CHECK(text.find("env.init.tdz") == std::string::npos);
}

TEST_CASE("a module-level lexical binding a function reads is checked at the read") {
    // The temporal half. `later` is written below `early`, and `early` is
    // lowered as a module function that loads the module record — so nothing
    // about the READ's position says whether the binding has been initialized,
    // and the check has to be at the read rather than elided by lowering having
    // walked past the declaration.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function early() { return later; }\n"
        "let later = 1;\n"
        "console.log(early());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("env.init.tdz") != std::string::npos);
    CHECK(text.find("env.get.tdz") != std::string::npos);
    CHECK(text.find("\"later\"") != std::string::npos);
}

TEST_CASE("an assignment to a lexical slot is checked before it stores") {
    // 6.2.5.6 PutValue reaches SetMutableBinding, which refuses an
    // uninitialized binding exactly as a read does — so the store is preceded
    // by the same checked read, whose result nothing uses.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let n = 0;\n"
        "function bump() { n = n + 1; }\n"
        "bump();\n"
        "console.log(n);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    const size_t checked = text.find("env.get.tdz");
    REQUIRE(checked != std::string::npos);
    CHECK(text.find("env.set", checked) != std::string::npos);
}
