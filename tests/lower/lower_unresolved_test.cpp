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

TEST_CASE("an unresolvable reference compiles to name.resolve and one warning") {
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
    CHECK(il::print(*optMod).find("name.resolve \"x\"") != std::string::npos);
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

TEST_CASE("bare typeof of an unresolved name is quiet, and asks at run time") {
    // 13.5.3 step 1. Feature detection is the one position where a free name is
    // not a question about the environment — it is a question about whether
    // there IS one — so it is neither an error nor a warning.
    //
    // It is not a folded constant either, and that is the SOFT form's whole
    // point: 9.1.1.4 makes a property of `globalThis` a global binding, so
    // `globalThis.__DEVTOOLS__ = {}` makes this `"object"`, and only the
    // run-time probe can know. The soft flag is what keeps the miss
    // `"undefined"` instead of the ReferenceError every other position raises.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "console.log(typeof __DEVTOOLS__);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK(diags.all().empty());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("name.resolve \"__DEVTOOLS__\", soft") != std::string::npos);
    CHECK(printed.find("typeof") != std::string::npos);
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
    CHECK(il::print(*optMod).find("name.resolve \"__DEVTOOLS__\"") != std::string::npos);
}

TEST_CASE("a named function expression names itself through an env binding") {
    // Supersedes "a named function expression cannot name itself, and says so",
    // which pinned the refusal this replaces. 15.2.5 binds the name in a
    // declarative environment created AROUND the function, so the self
    // reference is an ordinary capture — an `env.get`, not the `name.resolve` an
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
    CHECK(text.find("name.resolve \"rec\"") == std::string::npos);
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
    CHECK(unknownIl.find("name.resolve \"Maths\"") != std::string::npos);
    CHECK(unknownIl.find("global.get \"Maths\"") == std::string::npos);
}

TEST_CASE("`eval` is refused by name, in both the direct and the indirect spelling") {
    // The one name at the bottom of the ladder that is an ERROR rather than a
    // warning. 19.2.1 makes `eval`'s argument SOURCE TEXT compiled at run time,
    // and bronze links no compiler into the program, so there is nothing it
    // could resolve to and nothing a host could register that would be it —
    // `unresolved name 'eval': a ReferenceError if it is evaluated` would be
    // both true and useless, because it sends the reader looking for a binding.
    DiagnosticSink direct;
    SourceBuffer directBuf("direct.ts", "");
    parseAndLower("const v = eval(\"1 + 1\");\n", direct, directBuf);
    CHECK(direct.hasErrors());
    CHECK(direct.render(directBuf).find("`eval` is not implemented") != std::string::npos);

    // The diagnostic hangs off the READ of the name, not off a call shape, so
    // the indirect form — the one that runs in global scope in real engines —
    // is the same error. It is also what a bundler emits.
    DiagnosticSink indirect;
    SourceBuffer indirectBuf("indirect.ts", "");
    parseAndLower("const e = eval;\ne(\"1 + 1\");\n", indirect, indirectBuf);
    CHECK(indirect.hasErrors());
    CHECK(indirect.render(indirectBuf).find("`eval` is not implemented") != std::string::npos);

    DiagnosticSink aliased;
    SourceBuffer aliasedBuf("aliased.ts", "");
    parseAndLower("const o = { run: eval };\n", aliased, aliasedBuf);
    CHECK(aliased.hasErrors());

    // Not a ReferenceError at run time, either: the program does not build, so
    // no `name.resolve \"eval\"` is reachable to be caught by a `try`.
    DiagnosticSink guarded;
    SourceBuffer guardedBuf("guarded.ts", "");
    parseAndLower("try { eval(\"x\"); } catch (e) { }\n", guarded, guardedBuf);
    CHECK(guarded.hasErrors());
}

TEST_CASE("bare `typeof eval` stays the quiet feature-detection answer") {
    // The refusal must not eat the idiom that asks whether eval EXISTS.
    // `typeof eval === 'function'` is how code guards a dynamic-code path, and
    // a program that guards it correctly is a program bronze should compile —
    // taking the other branch is the whole point. 13.5.3 step 1 gives that
    // branch the right value without evaluating the reference, so the eval
    // error never fires: it lives past the `typeof` path, at the read.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "if (typeof eval === 'function') { console.log('dynamic'); }\n"
        "console.log('static');\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK(diags.all().empty());
    // The SOFT probe and never the raising one: `typeof eval` must not become
    // a read of `eval`, which is what the refusal above is attached to.
    CHECK(il::print(*optMod).find("name.resolve \"eval\", soft") != std::string::npos);
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
