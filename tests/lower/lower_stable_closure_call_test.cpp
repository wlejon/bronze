// The static call plan (`src/lower/lower_scope.cpp planStableFunctionSlots`):
// which calls to a sibling closure compile to a DIRECT call, and — far more
// importantly — which ones do not.
//
// This is NOT the direct METHOD edge's kind of claim. A method target is a
// guess the backend spends on a compare, so a wrong one costs a fast path and
// nothing else. This one is a PROOF, spent on no compare at all: the call
// enters the named function with an environment derived by counting parent
// links, and the closure value in the slot is never read. So every case below
// where the answer is "no edge" is a case where an edge would be a miscompile,
// and they are the point of the file.
//
// `env+N` in the printed IL is what the claim IS — the field exists only to be
// read by the backend, and no other output distinguishes a call that carries
// one.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;

namespace {

std::string lowerToText(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto mod = inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    return il::print(*mod);
}

// Whether some call names `@fn` and supplies an environment for it.
bool hasClosureEdge(const std::string& il, const std::string& fn) {
    return il.find("call @" + fn + "(env+") != std::string::npos;
}

size_t closureEdges(const std::string& il) {
    size_t n = 0;
    size_t at = il.find("(env+");
    while (at != std::string::npos) {
        ++n;
        at = il.find("(env+", at + 1);
    }
    return n;
}

}  // namespace

TEST_CASE("a sibling closure call names its target and derives the environment") {
    const auto il = lowerToText(
        "function make() {\n"
        "  let n = 0;\n"
        "  function bump(by) { n = n + by; }\n"
        "  function run(k) { bump(k); return n; }\n"
        "  return run;\n"
        "}\n"
        "console.log(make()(2));\n");
    CHECK(hasClosureEdge(il, "bump"));
    // Depth zero: `run` has no record of its own, so its `__env` IS the record
    // holding `bump`, and the call forwards it with no walk at all.
    CHECK(il.find("call @bump(env+0") != std::string::npos);
}

TEST_CASE("a call from deeper counts the parent links to the callee's record") {
    const auto il = lowerToText(
        "function outer() {\n"
        "  let tag = 1;\n"
        "  function help(x) { return tag + x; }\n"
        "  function middle() {\n"
        "    let inner = 2;\n"
        "    function local(x) { return x + inner; }\n"
        "    function deep() { return help(local(3)); }\n"
        "    return deep();\n"
        "  }\n"
        "  return middle();\n"
        "}\n"
        "console.log(outer());\n");
    // `local` is a binding of `middle`'s record, `help` one of `outer`'s.
    CHECK(il.find("call @local(env+0") != std::string::npos);
    CHECK(il.find("call @help(env+1") != std::string::npos);
}

TEST_CASE("a reassigned function binding gets no edge") {
    const auto il = lowerToText(
        "function outer() {\n"
        "  function greet() { return 1; }\n"
        "  function swap() { greet = function () { return 2; }; }\n"
        "  function ask() { return greet(); }\n"
        "  swap();\n"
        "  return ask();\n"
        "}\n"
        "console.log(outer());\n");
    CHECK_FALSE(hasClosureEdge(il, "greet"));
}

TEST_CASE("the reassignment may be anywhere in the scope's lexical reach") {
    // Three levels down, inside a class method, inside a nested closure. The
    // walk that answers this has to cross every one of those boundaries — the
    // SSA-sizing walk beside it deliberately does not.
    const auto il = lowerToText(
        "function outer() {\n"
        "  function greet() { return 1; }\n"
        "  function ask() { return greet(); }\n"
        "  function arm() {\n"
        "    return class {\n"
        "      go() { return (() => { greet = function () { return 2; }; })(); }\n"
        "    };\n"
        "  }\n"
        "  new (arm())().go();\n"
        "  return ask();\n"
        "}\n"
        "console.log(outer());\n");
    CHECK_FALSE(hasClosureEdge(il, "greet"));
}

TEST_CASE("a declaration lowered after the call site gets no edge") {
    // `isEven` is lowered first, so `isOdd` does not exist yet when its body
    // is; the reverse edge is available and is taken. It is also what keeps
    // the always-inline ask acyclic: an edge can only point at a function
    // whose lowering already finished.
    const auto il = lowerToText(
        "function parity(n) {\n"
        "  function isEven(k) { return k === 0 ? true : isOdd(k - 1); }\n"
        "  function isOdd(k) { return k === 0 ? false : isEven(k - 1); }\n"
        "  return isEven(n);\n"
        "}\n"
        "console.log(parity(4));\n");
    CHECK_FALSE(hasClosureEdge(il, "isOdd"));
    CHECK(hasClosureEdge(il, "isEven"));
}

TEST_CASE("a self-call gets no edge, and the sibling call beside it does") {
    // The slot is still empty while `fact`'s own body is being lowered, so the
    // recursive call takes the dynamic path; `go`, lowered afterwards, gets the
    // edge. One edge in the module, and it is not the recursive one.
    const auto il = lowerToText(
        "function outer() {\n"
        "  function fact(n) { return n < 2 ? 1 : n * fact(n - 1); }\n"
        "  function go() { return fact(5); }\n"
        "  return go();\n"
        "}\n"
        "console.log(outer());\n");
    // One edge: `go` -> `fact`. `outer` -> `go` is not one, because nothing
    // captures `go`, so it never gets an environment slot at all and its value
    // stays in SSA — a binding this plan says nothing about.
    CHECK_EQ(closureEdges(il), 1u);
    CHECK(il.find("call @fact(env+0") != std::string::npos);
}

TEST_CASE("a generator or async declaration gets no edge") {
    const auto il = lowerToText(
        "function outer() {\n"
        "  function* gen() { yield 1; }\n"
        "  function drive() { return gen().next().value; }\n"
        "  return drive();\n"
        "}\n"
        "console.log(outer());\n");
    CHECK_EQ(closureEdges(il), 0u);
}

TEST_CASE("a callee that needs an arguments object gets no edge") {
    const auto il = lowerToText(
        "function outer() {\n"
        "  function sum() {\n"
        "    let t = 0;\n"
        "    for (let i = 0; i < arguments.length; i++) { t = t + arguments[i]; }\n"
        "    return t;\n"
        "  }\n"
        "  function go() { return sum(1, 2, 3); }\n"
        "  return go();\n"
        "}\n"
        "console.log(outer());\n");
    CHECK_FALSE(hasClosureEdge(il, "sum"));
}

TEST_CASE("a shadowing binding of the same name refuses the whole slot") {
    // The refusal is NAME-based, so an inner `const pick` of its own costs the
    // outer declaration its edge. That is the over-approximation stated on
    // `getDeeplyAssignedNames`, and it is the safe direction: the cost is a
    // lost edge, never a call to the wrong function.
    const auto il = lowerToText(
        "function outer() {\n"
        "  function pick() { return 1; }\n"
        "  function inner() { const pick = function () { return 2; }; return pick(); }\n"
        "  function ask() { return pick(); }\n"
        "  return inner() + ask();\n"
        "}\n"
        "console.log(outer());\n");
    CHECK_FALSE(hasClosureEdge(il, "pick"));
}

TEST_CASE("a parameter default that writes the name refuses the slot") {
    // A default is code of this scope that the statement list does not
    // contain, so the plan has to be asked about it separately.
    const auto il = lowerToText(
        "function outer(seed = (target = function () { return 9; })) {\n"
        "  function target() { return 1; }\n"
        "  function ask() { return target(); }\n"
        "  return ask() + (seed === undefined ? 0 : 0);\n"
        "}\n"
        "console.log(outer());\n");
    CHECK_FALSE(hasClosureEdge(il, "target"));
}

TEST_CASE("a top-level declaration keeps the environment-free direct call") {
    // The path this stage did not touch: a module function needs no
    // environment, so its direct call carries no `env+` and the verifier's
    // original rule still holds over it.
    const auto il = lowerToText(
        "function add(a, b) { return a + b; }\n"
        "console.log(add(1, 2));\n");
    CHECK(il.find("call @add(") != std::string::npos);
    CHECK_EQ(closureEdges(il), 0u);
}

TEST_CASE("a short call takes the edge and an over-long one does not") {
    // Padding a short call is a compile-time fact, so `three(1)` spells its
    // operand list. `three(1, 2, 3, 4)` cannot: the extra argument is evaluated
    // and dropped, and a fixed operand list has nowhere to put it — the same
    // refusal `directCallShapeFits` has always made for a module function.
    const auto il = lowerToText(
        "function outer() {\n"
        "  let seen = 0;\n"
        "  function three(a, b, c) { seen = seen + 1; return \"\" + a + b + c; }\n"
        "  function go() { return three(1) + three(1, 2, 3, 4); }\n"
        "  return go();\n"
        "}\n"
        "console.log(outer());\n");
    CHECK(il.find("call @three(env+0") != std::string::npos);
    CHECK_EQ(closureEdges(il), 1u);  // the short call only
}
