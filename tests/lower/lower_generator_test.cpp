// The generator state machine, as `src/lower/lower_generator.cpp` builds it.
//
// One generator becomes TWO IL functions: the factory the call site invokes,
// which runs none of the body and hands back a generator object, and the resume
// function that IS the body, entered at its top on the first `next` and just
// after a `yield` on every later one. The facts worth pinning here are the ones
// the oracle's stdout cannot show — that no part of the body runs before the
// first `next`, that the resume entry really is a dispatch on a parked index,
// and above all that nothing the body needs is carried in an SSA value: the
// resume edge jumps into the middle of the function and defines nothing, so a
// local that survives a suspension has to come out of the environment record.
//
// Whether the machine also produces the right VALUES is
// tests/oracle/cases/generator_*.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

namespace {

size_t countOf(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

const il::Function* functionNamed(const il::Module& mod, const std::string& name) {
    for (const auto& fn : mod.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

// The text of one function only, so a `find` cannot be satisfied by a match in
// a sibling — the whole point of most assertions below is WHICH of the two
// functions an instruction landed in. Sliced out of the whole module's text
// rather than printed from a one-function copy, because an operand that names
// another function or a binding resolves against the module.
std::string textOf(const il::Module& mod, const std::string& name) {
    const std::string all = il::print(mod);
    const std::string head = "func " + name + "(";
    const size_t at = all.find(head);
    if (at == std::string::npos) return "";
    const size_t end = all.find("\nfunc ", at);
    return all.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

TEST_CASE("a generator lowers to a factory and a resume function") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("function* g() { yield 1; }\nconst it = g();\n", diags, buf);
    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());

    REQUIRE(functionNamed(*optMod, "g") != nullptr);
    REQUIRE(functionNamed(*optMod, "g.resume") != nullptr);

    // The resume function's signature is the whole calling convention: the
    // frame record, how it is being resumed (next / return / throw), and the
    // value that resumption carries.
    CHECK(functionNamed(*optMod, "g.resume")->params.size() == 3);

    // 27.5.1.2: calling a generator function runs NONE of its body. The factory
    // builds the frame, closes over it, and returns the object.
    const std::string factory = textOf(*optMod, "g");
    CHECK(factory.find("env.create") != std::string::npos);
    CHECK(factory.find("create.func @g.resume") != std::string::npos);
    CHECK(factory.find("create.generator_object") != std::string::npos);
    // Nothing of the body: no result object is built here, and no yield returns
    // from here.
    CHECK(factory.find("create.object") == std::string::npos);
    CHECK(countOf(factory, "ret ") == 1);

    // Conversely the resume function never builds a generator object: it is the
    // body, not the constructor.
    CHECK(textOf(*optMod, "g.resume").find("create.generator_object") == std::string::npos);
}

TEST_CASE("the resume entry dispatches on a parked state index") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        inferAndLower("function* g() { yield 1; yield 2; yield 3; }\nconst it = g();\n", diags, buf);
    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string resume = textOf(*optMod, "g.resume");

    // The state lives in the frame, so it survives the return between two
    // calls; it is read once, at the top, and compared against each resume
    // point in turn.
    CHECK(resume.find("env.get %0, 0, 0") != std::string::npos);
    // Three yields plus the start: four entries in the dispatch, so three
    // equality tests against the read index (start is the fall-through 0).
    CHECK(countOf(resume, "cmp.eq") >= 3);

    // Every suspension parks an index and returns; the last thing a `yield`
    // does is `ret`, because a suspension IS a return in this design.
    CHECK(countOf(resume, "ret ") >= 4);
}

TEST_CASE("a yield returns an iterator result, and the final one says done") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("function* g() { yield 1; }\nconst it = g();\n", diags, buf);
    REQUIRE(optMod.has_value());
    const std::string resume = textOf(*optMod, "g.resume");

    // 7.4.1 CreateIterResultObject: an ordinary object with `value` then
    // `done`, built at every exit — the suspension, the two abrupt resumptions,
    // and falling off the end.
    CHECK(countOf(resume, "create.object") == countOf(resume, "const.bool"));
    CHECK(resume.find("const.bool false") != std::string::npos);
    CHECK(resume.find("const.bool true") != std::string::npos);
}

TEST_CASE("a binding that crosses a yield lives in the frame, not in SSA") {
    // The core soundness property. `i` is written before the suspension and
    // read after it, and the read happens in a block reached by an edge from
    // the dispatch — an edge that defines nothing. If `i` were an SSA value the
    // second block would be referring to a definition that does not reach it.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function* g() { let i = 41; yield i; i = i + 1; return i; }\nconst it = g();\n", diags,
        buf);
    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string resume = textOf(*optMod, "g.resume");

    // Read out of the record on both sides of the suspension, never carried.
    CHECK(countOf(resume, "\"i\"") >= 3);
    CHECK(resume.find("env.set %0") != std::string::npos);

    // The frame record is the resume function's FIRST PARAMETER, so every one
    // of those accesses is at depth 0 from a value the entry block owns. A
    // depth-0 access to anything else would mean lowering had re-derived the
    // record from a value the resume edge skipped over. `i` is a `let`, so its
    // reads are the TDZ-checking form.
    CHECK(countOf(resume, "env.get.tdz %0, 0,") >= 2);
}

TEST_CASE("a nested scope's record is reachable downward from the frame") {
    // A `yield` inside a block means the record innermost at the suspension is
    // itself an SSA value the resume edge cannot see. The chain runs upward, so
    // the frame keeps a link DOWN to its child, and lowering re-derives the
    // inner record from the frame parameter at every use.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function* g() { for (let i = 0; i < 3; i++) { yield i; } }\nconst it = g();\n", diags, buf);
    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string resume = textOf(*optMod, "g.resume");

    // The link is written when the scope opens...
    CHECK(resume.find("env.create") != std::string::npos);
    // ...and read back from the frame parameter, not from whatever value
    // `env.create` produced, every time the inner record is needed.
    CHECK(countOf(resume, "env.get %0, 0,") >= 2);
}

TEST_CASE("a generator returns dynamic whatever inference proved about the body") {
    // Inference reasons about the body's `return`, but a generator function
    // does not return that: it returns a generator object. The IL signature has
    // to say so, or the factory's `ret` would be typed against the wrong thing.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        inferAndLower("function* g() { yield 1; return 2; }\nconst it = g();\n", diags, buf);
    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    CHECK(textOf(*optMod, "g").find("func g() -> dynamic") != std::string::npos);
}

TEST_CASE("the state machine is the same shape with and without inference") {
    // Suspension is not an optimization: `--no-infer` must build the same
    // machine, because the two paths have to agree byte-for-byte on stdout.
    const char* src = "function* g() { let i = 0; while (i < 3) { yield i; i = i + 1; } }\n"
                      "const it = g();\n";
    DiagnosticSink d1;
    SourceBuffer b1("test.ts", "");
    const auto inferred = inferAndLower(src, d1, b1);
    DiagnosticSink d2;
    SourceBuffer b2("test.ts", "");
    const auto plain = parseAndLower(src, d2, b2);
    REQUIRE(inferred.has_value());
    REQUIRE(plain.has_value());
    REQUIRE_FALSE(d1.hasErrors());
    REQUIRE_FALSE(d2.hasErrors());

    for (const auto& mod : {inferred, plain}) {
        CHECK(functionNamed(*mod, "g.resume") != nullptr);
        CHECK(textOf(*mod, "g").find("create.generator_object") != std::string::npos);
        CHECK(textOf(*mod, "g.resume").find("create.generator_object") == std::string::npos);
    }
}

TEST_CASE("an ordinary function is untouched by any of this") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("function f(a) { return a + 1; }\nconsole.log(f(1));\n",
                                      diags, buf);
    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find(".resume") == std::string::npos);
    CHECK(text.find("create.generator_object") == std::string::npos);
    CHECK(functionNamed(*optMod, "f") != nullptr);
}
