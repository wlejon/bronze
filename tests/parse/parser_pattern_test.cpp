// Binding patterns and the parameter forms around them, named for
// `src/parse/parser_pattern.cpp`. A pattern is what stands where a binding name
// would — in a declaration, in a parameter, in a for-of head — so it nests, and
// the dump is where the nesting is checked.

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

TEST_CASE("ES2015 parameter and spread syntax parses into nodes of its own") {
    // Each form must reach a DISTINCT node, because each lowers differently: a
    // default is a branch, a rest is an array the convention builds, a spread
    // is a walk over a container, a pattern is a read per element. Dumping any
    // two of them alike would hide a wrong lowering.
    struct Case {
        const char* src;
        const char* expected;
    };
    const Case cases[] = {
        {"function f(a, b = 2) {}", "(param b"},
        {"const g = (a = 1) => a;", "(param a"},
        {"function f(...r) {}", "(function f (...r)"},
        {"class C { m(...r) {} }", "(function-expr C.m (...r)"},
        {"function h([a]) {}", "(pattern-array"},
        {"function h({a}) {}", "(pattern-object"},
        {"const [a, b] = [1, 2];", "(const <pattern>"},
        {"const { x } = { x: 1 };", "(pattern-object"},
        {"f(...[1]);", "(spread"},
        {"const c = [...[1]];", "(spread"},
        {"const o = { ...x };", "(prop-spread"},
        {"[a, b] = [b, a];", "(destructuring-assign"},
        {"({ x } = o);", "(destructuring-assign"},
    };
    for (const auto& c : cases) {
        const auto out = parseAndDump(c.src);
        CHECK(out.substr(0, 7) != "ERRORS:");
        CHECK(out.find(c.expected) != std::string::npos);
    }
}

TEST_CASE("a pattern nests, renames, defaults and rests, and dumps each apart") {
    const auto out = parseAndDump("function h([a, [b], ...c], { d, e: f = 1, ...g }) {}");
    CHECK(out ==
          "(module t\n"
          "  (function h (<pattern> <pattern>)\n"
          "    (param <pattern>\n"
          "      (pattern-array\n"
          "        (elem a\n"
          "        )\n"
          "        (elem <pattern>\n"
          "          (pattern-array\n"
          "            (elem b\n"
          "            )\n"
          "          )\n"
          "        )\n"
          "        (elem ...c\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "    (param <pattern>\n"
          "      (pattern-object\n"
          "        (elem d: d\n"
          "        )\n"
          "        (elem e: f\n"
          "          (default\n"
          "            (number 1)\n"
          "          )\n"
          "        )\n"
          "        (elem ...g\n"
          "        )\n"
          "      )\n"
          "    )\n"
          "  )\n"
          ")\n");
}

TEST_CASE("the syntax around patterns that ECMA-262 forbids is named") {
    // Everything the grammar rules out, plus the two constructs bronze
    // deliberately does not build. A silent acceptance here is a wrong answer,
    // not a missing feature.
    struct Case {
        const char* src;
        const char* expected;
    };
    const Case cases[] = {
        {"function f(...r, a) {}", "a rest parameter must be the last parameter"},
        {"function f(...r = 1) {}", "a rest parameter may not have a default value"},
        {"function f(...[a]) {}", "a rest parameter must be a plain name"},
        {"const [a, ...r, b] = x;", "a rest element must be the last element of an array pattern"},
        {"const { ...r, a } = x;", "a rest property must be the last element of an object pattern"},
        {"const { ...[a] } = x;", "expected a name after '...' in an object pattern"},
        {"let [a];", "a destructuring declaration requires an initializer"},
        {"const [a, , b] = x;", "unsupported construct: an elision (a hole) in an array pattern"},
        {"const c = [1, , 2];", "unsupported construct: an elision (a hole) in an array literal"},
        {"[o.x] = y;",
         "unsupported construct: a destructuring assignment target that is not a name or a "
         "nested pattern"},
        {"[a] += b;", "a destructuring pattern may only be the target of '='"},
        {"const x = ...y;", "'...' is only allowed in an argument list"},
    };
    for (const auto& c : cases) {
        const auto out = parseAndDump(c.src);
        CHECK(out.substr(0, 7) == "ERRORS:");
        CHECK(out.find(c.expected) != std::string::npos);
    }
}

TEST_CASE("a cover-initialized name parses, and dumps as the pattern-only form it is") {
    // `{ x = 1 }` is legal ONLY as the left of a `=` (ECMA-262 13.2.5.1), and
    // the parser cannot know which it is until it reads on. So it parses, and
    // dumps under a head of its own — the error for the literal reading of it
    // belongs to lowering, which is the first pass that knows no `=` came.
    const auto cover = parseAndDump("const o = { x = 1 };");
    CHECK(cover.substr(0, 7) != "ERRORS:");
    CHECK(cover.find("(prop-cover-init x") != std::string::npos);

    const auto refined = parseAndDump("({ x = 1 } = o);");
    CHECK(refined.substr(0, 7) != "ERRORS:");
    CHECK(refined.find("(destructuring-assign") != std::string::npos);
    CHECK(refined.find("(default") != std::string::npos);
}
