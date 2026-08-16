// The identifier resolution ladder and its end: the seam
// `src/lower/lower_unresolved.cpp` implements. A name that reaches the bottom
// of the ladder is not a compile error but a ReferenceError at the moment of
// use, with a compile-time warning — except under a bare `typeof`, and except
// when it is a name bronze DECLARED and failed to bind, which stays bronze's
// own hard error.

#include <doctest/doctest.h>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

TEST_CASE("an unresolvable reference compiles to ref.error and one warning") {
    // Supersedes "undefined variable reference generates diagnostic error".
    // What a free name denotes is a fact only the running environment holds, so
    // refusing the program was the wrong hard error: 6.2.5.5 puts the
    // ReferenceError at the moment of USE. The build is still loud — a warning
    // names the identifier — and the program still fails, at the point the
    // language says it does.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const y = x + 1;\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("warning: unresolved name 'x'") != std::string::npos);
    CHECK(il::print(*optMod).find("ref.error \"x\"") != std::string::npos);
}

TEST_CASE("one warning per unresolved NAME, however many mentions") {
    // `document` appears eleven times in three.js's utils.js, and eleven
    // identical warnings is a diagnostic nobody reads.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f() { return doc.a + doc.b + doc.c; }\n"
        "console.log(typeof f);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
    size_t count = 0;
    for (const auto& d : diags.all()) {
        if (d.message.find("'doc'") != std::string::npos) ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("bare typeof of an unresolvable name is a string, with no diagnostic") {
    // 13.5.3 step 1. Feature detection is the one position where a free name is
    // not a question about the environment — it is a question about whether
    // there IS one — so it is neither an error nor a warning. Not even a
    // `typeof` instruction: the answer is a constant.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "console.log(typeof __DEVTOOLS__);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK(diags.all().empty());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("ref.error") == std::string::npos);
    CHECK(printed.find("typeof") == std::string::npos);
}

TEST_CASE("typeof of a MEMBER of an unresolvable name still throws") {
    // 13.5.3's exemption is for an unresolvable REFERENCE, which `x.y` is not:
    // evaluating it evaluates `x` first, and that is a GetValue.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "console.log(typeof __DEVTOOLS__.version);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK(il::print(*optMod).find("ref.error \"__DEVTOOLS__\"") != std::string::npos);
}

TEST_CASE("a named function expression names itself through an env binding") {
    // Supersedes "a named function expression cannot name itself, and says so",
    // which pinned the refusal this replaces. 15.2.5 binds the name in a
    // declarative environment created AROUND the function, so the self
    // reference is an ordinary capture — an `env.get`, not the `ref.error` an
    // unresolvable name lowers to, and not a compile error either.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const f = function rec(n) { return n <= 0 ? 0 : rec(n - 1); };\n"
        "console.log(f(3));\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("ref.error \"rec\"") == std::string::npos);
    CHECK(text.find("env.get") != std::string::npos);
}

TEST_CASE("only STRICT code emits the immutable-assignment throw") {
    // 9.1.1.1.5 step 4 throws for an immutable binding in strict code and
    // returns quietly otherwise, so the sloppy write lowers to NOTHING at all.
    // An instruction that decided at runtime would put a helper call on every
    // write to a name that merely shares a spelling with the function.
    DiagnosticSink strictDiags;
    SourceBuffer strictBuf("strict.ts", "");
    const auto strictMod = parseAndLower(
        "\"use strict\";\n"
        "const f = function rec() { rec = 1; return 0; };\n"
        "console.log(f());\n",
        strictDiags, strictBuf);
    REQUIRE(strictMod.has_value());
    CHECK(il::print(*strictMod).find("immutable.assign \"rec\"") != std::string::npos);

    DiagnosticSink sloppyDiags;
    SourceBuffer sloppyBuf("sloppy.ts", "");
    const auto sloppyMod = parseAndLower(
        "const f = function rec() { rec = 1; return 0; };\n"
        "console.log(f());\n",
        sloppyDiags, sloppyBuf);
    REQUIRE(sloppyMod.has_value());
    CHECK(il::print(*sloppyMod).find("immutable.assign") == std::string::npos);
}

TEST_CASE("a `var` inside a block hoists to function scope and binds cleanly") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function g() { if (true) { var j = 6; } return j; }\n"
        "console.log(g());\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
}

TEST_CASE("a `var` at a function's top level still binds, and is not the error above") {
    // The guard must not widen into every `var`: the top-level form works, and
    // an over-eager check would refuse correct programs — which is the shape of
    // the for-loop capture bug that had to be undone.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function g() { var j = 6; return j; }\n"
        "console.log(g());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("a provided global resolves to global.get; an unknown free name does not") {
    // The globals list is still closed at COMPILE time: `Math` becomes an
    // instruction. What changed is the OTHER half — a name that is not on the
    // list is not thereby a compile error, because bronze cannot know what the
    // environment holds. It becomes the ReferenceError 6.2.5.5 raises when it
    // is EVALUATED, which is still not the runtime miss reading `undefined`
    // that the closed provided-globals list refuses.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("const r = Math.sqrt(2);\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("global.get \"Math\"") != std::string::npos);

    DiagnosticSink diags2;
    SourceBuffer buf2("test.ts", "");
    const auto unknown = parseAndLower("const r = Maths.sqrt(2);\n", diags2, buf2);
    CHECK_FALSE(diags2.hasErrors());
    REQUIRE(unknown.has_value());
    const std::string unknownIl = il::print(*unknown);
    CHECK(unknownIl.find("ref.error \"Maths\"") != std::string::npos);
    CHECK(unknownIl.find("global.get \"Maths\"") == std::string::npos);
}

TEST_CASE("a local binding shadows a provided global") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const Math = { sqrt: 1 };\n"
        "const r = Math.sqrt;\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("global.get") == std::string::npos);
}
