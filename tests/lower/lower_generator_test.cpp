// Generators and async functions as `src/lower/lower_generator.cpp` builds them.
//
// One coroutine becomes TWO IL functions: the stub the call site invokes, which
// is a single `coro.start` over the body with every parameter forwarded, and the
// body `<name>.body`, which is the function as written with a `coro.suspend` at
// each `yield` and `await`. brass's coroutine transform splits the body at
// those suspends and keeps every live value in the frame, so lowering builds no
// state machine: no dispatch, no frame slots, no bindings forced into an
// environment record. The facts worth pinning here are the ones the oracle's
// stdout cannot show — which function each instruction lands in, which suspend
// kind each source form produces, and that after a `yield` the resume mode is
// read and dispatched on.
//
// Whether the coroutines also produce the right VALUES is
// tests/oracle/cases/generator_* and async_*.

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
// a sibling — most assertions below are about WHICH of the two functions an
// instruction landed in.
std::string textOf(const il::Module& mod, const std::string& name) {
    const std::string all = il::print(mod);
    const std::string head = "func " + name + "(";
    const size_t at = all.find(head);
    if (at == std::string::npos) return "";
    const size_t end = all.find("\nfunc ", at);
    return all.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::optional<il::Module> lowered(const char* src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = inferAndLower(src, diags, buf);
    REQUIRE(mod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    return mod;
}

}  // namespace

TEST_CASE("a generator lowers to a coro.start stub and a coroutine body") {
    const auto mod = lowered("function* g(a) { yield a; }\nconst it = g(1);\n");

    const il::Function* stub = functionNamed(*mod, "g");
    const il::Function* body = functionNamed(*mod, "g.body");
    REQUIRE(stub != nullptr);
    REQUIRE(body != nullptr);
    CHECK_FALSE(stub->isCoroutineBody());
    CHECK(body->isCoroutineBody());
    CHECK(body->coroKind == BRONZE_ABI_CORO_GENERATOR);

    // The stub takes exactly what the body takes, and forwards all of it: the
    // body is the function as written, hidden parameters included.
    CHECK(stub->params.size() == body->params.size());
    const std::string stubText = textOf(*mod, "g");
    CHECK(stubText.find("coro.start generator @g.body") != std::string::npos);
    CHECK(countOf(stubText, "coro.suspend") == 0);

    // No part of the body runs before the first `next`: the body parks at a
    // START suspend after its prologue, before the first user statement.
    const std::string bodyText = textOf(*mod, "g.body");
    CHECK(bodyText.find("coro.suspend start") != std::string::npos);
    CHECK(bodyText.find("coro.suspend yield") != std::string::npos);
    CHECK(bodyText.find("coro.start") == std::string::npos);
}

TEST_CASE("after a yield the resume mode is read and dispatched on") {
    const auto mod = lowered("function* g() { const x = yield 1; return x; }\nconst it = g();\n");
    const std::string body = textOf(*mod, "g.body");
    // One suspend per yield plus the start; the yield is followed by a mode
    // read and two tests (throw, return). The start suspend reads no mode: a
    // `throw()` or `return()` on a suspendedStart generator never resumes it.
    CHECK(countOf(body, "coro.suspend") == 2);
    CHECK(countOf(body, "coro.mode") == 1);
    CHECK(body.find("throw") != std::string::npos);
    // Nothing crosses the yield through an environment record: `x` is SSA.
    CHECK(body.find("env.set") == std::string::npos);
}

TEST_CASE("an async function awaits with an await suspend and no start suspend") {
    const auto mod = lowered("async function f(p) { const v = await p; return v; }\nf(1);\n");
    const il::Function* body = functionNamed(*mod, "f.body");
    REQUIRE(body != nullptr);
    CHECK(body->coroKind == BRONZE_ABI_CORO_ASYNC);
    CHECK(textOf(*mod, "f").find("coro.start async @f.body") != std::string::npos);
    const std::string text = textOf(*mod, "f.body");
    // An async body runs synchronously up to its first await.
    CHECK(text.find("coro.suspend start") == std::string::npos);
    CHECK(countOf(text, "coro.suspend await") == 1);
    CHECK(countOf(text, "coro.mode") == 1);
}

TEST_CASE("an async generator starts parked and awaits what it yields") {
    const auto mod = lowered("async function* g(p) { yield p; }\ng(1);\n");
    const il::Function* body = functionNamed(*mod, "g.body");
    REQUIRE(body != nullptr);
    CHECK(body->coroKind == BRONZE_ABI_CORO_ASYNC_GENERATOR);
    const std::string text = textOf(*mod, "g.body");
    CHECK(text.find("coro.suspend start") != std::string::npos);
    // 27.6.3.8 AsyncGeneratorYield awaits its operand before yielding it.
    CHECK(text.find("coro.suspend await") != std::string::npos);
    CHECK(text.find("coro.suspend yield") != std::string::npos);
}

TEST_CASE("a sync `yield*` passes the inner result through a delegate suspend") {
    const auto mod = lowered("function* g(xs) { yield* xs; }\nconst it = g([1]);\n");
    const std::string text = textOf(*mod, "g.body");
    CHECK(countOf(text, "iter.open") == 1);
    CHECK(countOf(text, "iter.delegate") == 1);
    CHECK(countOf(text, "coro.suspend delegate") == 1);
    CHECK(text.find("coro.suspend yield") == std::string::npos);

    const auto plain = lowered("function* g() { yield 1; }\nconst it = g();\n");
    CHECK(textOf(*plain, "g.body").find("iter.delegate") == std::string::npos);
}

TEST_CASE("a generator returns dynamic whatever inference proved about the body") {
    // Inference reasons about the body's `return`, but a generator function
    // does not return that: it returns a generator object, and the body's
    // `ret` is the completion value brass hands the resumer.
    const auto mod = lowered("function* g() { yield 1; return 2; }\nconst it = g();\n");
    CHECK(textOf(*mod, "g").find("-> dynamic") != std::string::npos);
    CHECK(textOf(*mod, "g.body").find("-> dynamic") != std::string::npos);
}

TEST_CASE("the coroutine is the same shape with and without inference") {
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
        CHECK(functionNamed(*mod, "g.body") != nullptr);
        CHECK(textOf(*mod, "g").find("coro.start generator") != std::string::npos);
        CHECK(countOf(textOf(*mod, "g.body"), "coro.suspend") == 2);
    }
}

TEST_CASE("an ordinary function is untouched by any of this") {
    const auto mod = lowered("function f(a) { return a + 1; }\nconsole.log(f(1));\n");
    const std::string text = il::print(*mod);
    CHECK(text.find(".body(") == std::string::npos);
    CHECK(text.find("coro.") == std::string::npos);
    const il::Function* f = functionNamed(*mod, "f");
    REQUIRE(f != nullptr);
    CHECK_FALSE(f->isCoroutineBody());
}
