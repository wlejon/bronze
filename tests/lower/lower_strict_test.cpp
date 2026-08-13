// Strict mode's one effect on the IL: the flag that rides on every write and
// delete instruction, which is what 13.15.2 PutValue step 6.d reads to decide
// whether a refused Set throws or is discarded.
//
// It is asserted on the printed IL rather than through the runtime, because
// that is where the question "which mode is this write in?" has an answer that
// can be wrong without anything crashing — a sloppy flag on a strict write
// produces a program that runs fine and gives the wrong answer, which is
// exactly the class of bug a canonical dump exists to catch.
//
// The flag is per-INSTRUCTION rather than per-IL-function, and these tests are
// why: one module holds strict and sloppy bodies at once, and `main` holds the
// script's own top level, which is a third body again.

#include <doctest/doctest.h>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::parseAndLower;

namespace {

// The IL on the `--no-infer` path. Strictness is a parse-time fact and
// inference has no opinion about it, so the dynamic path shows the flag with
// the least around it.
std::string lowerToIl(std::string_view src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    const auto mod = parseAndLower(src, diags, buf);
    if (diags.hasErrors()) return "ERRORS:\n" + diags.render(buf);
    REQUIRE(mod.has_value());
    return il::print(*mod);
}

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// One IL function's body, so that an assertion about a method cannot be
// satisfied by an instruction in `main`.
std::string functionBody(const std::string& il, const char* head) {
    const size_t begin = il.find(head);
    REQUIRE(begin != std::string::npos);
    const size_t end = il.find("\nfunc ", begin + 1);
    return il.substr(begin, end == std::string::npos ? end : end - begin);
}

}  // namespace

TEST_CASE("a property write carries the mode of the code it is written in") {
    // The same source twice, one directive apart. `prop.set obj, <key>, val,
    // <ic>, <strict>` — the last field is the whole difference.
    const std::string sloppy = lowerToIl("const p = { a: 0 };\np.a = 1;\n");
    const std::string strict = lowerToIl("\"use strict\";\nconst p = { a: 0 };\np.a = 1;\n");

    // The object literal's own definition is unflagged in BOTH: a literal
    // defines a property on an object it has just created, which cannot be
    // refused and is not a reference for strictness to be a property of.
    CHECK(has(sloppy, "prop.set %0, 0, %2, 0, 0\n"));
    CHECK(has(strict, "prop.set %1, 1, %3, 0, 0\n"));
    // The assignment after it is the only instruction that differs.
    CHECK(has(sloppy, "prop.set %0, 0, %4, 1, 0\n"));
    CHECK(has(strict, "prop.set %1, 1, %5, 1, 1\n"));
}

TEST_CASE("every write form carries it, and a read carries nothing") {
    const std::string il = lowerToIl(
        "\"use strict\";\n"
        "const o = {};\n"
        "const k = \"x\";\n"
        "o[k] = 1;\n"
        "const q = { k: 0 };\n"
        "q.k += 1;\n"
        "q.k++;\n"
        "delete q.k;\n");

    // A computed key is `elem.set obj, key, val, <strict>` — no inline cache,
    // so the flag is the fourth field rather than the fifth.
    CHECK(has(il, "elem.set %1, %2, %4, 1\n"));
    // A compound assignment is a read and a write, and only the write is
    // flagged: `prop.get` has no such field at all.
    CHECK(has(il, "%8: dynamic = prop.get %5, 2, 1\n"));
    CHECK(has(il, "prop.set %5, 2, %11, 2, 1\n"));
    // An update operator writes too (13.4.2.1).
    CHECK(has(il, "prop.set %5, 2, %16, 4, 1\n"));
    // And `delete` carries it, because 13.5.1.2 step 5.b turns a refusal into
    // a TypeError on the same rule.
    CHECK(has(il, "%17: bool = prop.delete %5, 2, 1\n"));
}

TEST_CASE("a sloppy delete is flagged 0") {
    CHECK(has(lowerToIl("const o = { k: 1 };\ndelete o.k;\n"), "prop.delete %0, 0, 0\n"));
}

TEST_CASE("three bodies in one module, three answers") {
    // A class method (strict by 15.7 with no directive anywhere), a function
    // with its own directive, a function without one, and the module's own top
    // level. Nothing but a per-instruction flag can express this module.
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    const auto mod = parseAndLower(
        "const o = { k: 0 };\n"
        "class C {\n"
        "  m() { o.k = 1; }\n"
        "}\n"
        "o.k = 2;\n"
        "function s() { \"use strict\"; o.k = 3; }\n"
        "function t() { o.k = 4; }\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    const std::string il = il::print(*mod);

    // This is the one test in the file whose source declares functions, so it
    // is the one whose key indices are not a fixed prefix — `k` sits behind
    // whatever the function names interned. Spelled through the lookup.
    const std::string k = std::to_string(bronze::lower_test::keyIndex(*mod, "k"));
    CHECK(has(functionBody(il, "func C.m"), ("prop.set %1, " + k + ", %3, 4, 1\n").c_str()));
    CHECK(has(functionBody(il, "func s()"), ("prop.set %2, " + k + ", %4, 0, 1\n").c_str()));
    CHECK(has(functionBody(il, "func t()"), ("prop.set %1, " + k + ", %3, 1, 0\n").c_str()));
    CHECK(has(functionBody(il, "func main()"), ("prop.set %8, " + k + ", %10, 5, 0\n").c_str()));
}

TEST_CASE("an ordinary call passes undefined as its this value") {
    // 13.3.6.1: a call with no base passes `undefined`. bronze has no global
    // object to substitute for it in sloppy mode, so this is its one answer in
    // every mode — and it happens to be the strict one, exactly as its one
    // answer for an unresolvable reference is.
    //
    // It used to emit a boxed ZERO here, so `const g = o.m; g()` ran with
    // `this === 0` while a direct call in the same program ran with `this`
    // undefined.
    const std::string il = lowerToIl("const f = function () { return 1; };\nf();\n");
    CHECK(has(functionBody(il, "func main()"),
              "    %2: dynamic = const.undefined\n"
              "    %3: dynamic = call.dynamic %1, %2, 0\n"));
}
