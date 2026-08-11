#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>

#include "lex/lexer.h"
#include "parse/parser.h"
#include "types/dump.h"
#include "types/infer.h"

using namespace bronze;

namespace {

// Keeps the AST alive for the duration of a test: the result side table is
// keyed by AST node, so nothing it answers is meaningful once the module is
// gone.
struct Inferred {
    SourceBuffer buffer{"test.js", ""};
    DiagnosticSink diags;
    std::unique_ptr<ast::Module> module;
    std::optional<types::InferenceResult> result;

    std::string dump() const { return types::dump(*result); }
};

Inferred infer(const std::string& source) {
    Inferred out;
    out.buffer = SourceBuffer("test.js", source);
    auto tokens = Lexer(out.buffer, out.diags).lex();
    REQUIRE_FALSE(out.diags.hasErrors());
    out.module = Parser(std::move(tokens), out.diags).parseModule("test");
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.module != nullptr);
    out.result = types::inferModule(*out.module, out.diags);
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.result.has_value());
    return out;
}

}  // namespace

// ---- the lattice ------------------------------------------------------------

TEST_CASE("join is Never-absorbing, idempotent, and Dynamic otherwise") {
    using types::Type;
    CHECK(join(Type::never(), Type::number()) == Type::number());
    CHECK(join(Type::number(), Type::never()) == Type::number());
    CHECK(join(Type::number(), Type::number()) == Type::number());
    CHECK(join(Type::number(), Type::string()) == Type::dynamic());
    CHECK(join(Type::undefined(), Type::number()) == Type::dynamic());
    CHECK(join(Type::dynamic(), Type::never()) == Type::dynamic());

    // Same kind, different identity: the kind survives, the identity does
    // not. This is decision 4's "Object with no class".
    CHECK(join(Type::object(0), Type::object(0)) == Type::object(0));
    CHECK(join(Type::object(0), Type::object(1)) == Type::object());
    CHECK(join(Type::object(0), Type::object(1)).shapeClass() == types::kNoShapeClass);
    CHECK(join(Type::function(0), Type::function(1)) == Type::function());
    CHECK(join(Type::object(0), Type::function(0)) == Type::dynamic());

    CHECK(Type::number().str() == "number");
    CHECK(Type::object().str() == "object");
    CHECK(Type::object(3).str() == "object#3");
    CHECK(Type::function(1).str() == "function#1");
    CHECK(Type::never().str() == "never");
}

// ---- flow analysis ----------------------------------------------------------

TEST_CASE("primitive literals and the operators that join them") {
    const auto inferred = infer(
        "let a = 1;\n"
        "let b = \"s\";\n"
        "let c = true;\n"
        "let d = null;\n"
        "let e;\n"
        "let f = a + 1;\n"
        "let g = b + a;\n"
        "let h = a - b;\n"
        "let i = a < 1;\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 let  a: number\n"
          "  #1 let  b: string\n"
          "  #2 let  c: bool\n"
          "  #3 let  d: null\n"
          "  #4 let  e: undefined\n"
          "  #5 let  f: number\n"
          // `+` concatenates as soon as either side is a string...
          "  #6 let  g: string\n"
          // ...but `-` is ToNumber on both sides, so it is a number even
          // when an operand is not (NaN is a number).
          "  #7 let  h: number\n"
          "  #8 let  i: bool\n");
}

TEST_CASE("a straight-line reassignment is flow-sensitive, not a collapse") {
    // Decision 2 says `number | undefined` collapses to Dynamic because
    // there are no unions; decision 3 says the narrow case of a binding
    // written once before any use is the flow analysis's job, not the
    // type's. Both are visible here: nothing collapses, because the two
    // writes never reach the same program point.
    const auto inferred = infer(
        "let x = 1;\n"
        "x = \"two\";\n"
        "let y;\n"
        "y = 3;\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 let  x: number\n"
          "  #1 expr  x: string\n"
          "  #2 let  y: undefined\n"
          "  #3 expr  y: number\n");
}

TEST_CASE("a let written to two types on two paths collapses to Dynamic at the merge") {
    const auto inferred = infer(
        "function pick(flag) {\n"
        "  let v = 1;\n"
        "  if (flag) {\n"
        "    v = 2;\n"
        "  } else {\n"
        "    v = \"two\";\n"
        "  }\n"
        "  return v;\n"
        "}\n"
        "pick(true);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          // The parameter comes from the one call site (decision 5).
          "func pick(flag: bool) -> dynamic direct-callable\n"
          "  #0 let  v: number\n"
          "  #1 if  v: dynamic\n"
          "    then\n"
          // `v = 2` leaves it a number, so the then-arm changes nothing.
          "      #0 expr\n"
          "    else\n"
          "      #0 expr  v: string\n"
          "  #2 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 expr\n");
}

TEST_CASE("a loop iterates to fixpoint") {
    // `v` is a number on entry and a string at the end of the body, so the
    // loop header — and therefore everything after the loop — is Dynamic.
    // A single forward walk would have said "number" and been wrong.
    const auto inferred = infer(
        "let v = 1;\n"
        "let i = 0;\n"
        "while (i < 3) {\n"
        "  v = \"s\";\n"
        "  i = i + 1;\n"
        "}\n"
        "let after = v;\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 let  v: number\n"
          "  #1 let  i: number\n"
          "  #2 while  v: dynamic\n"
          "    body\n"
          "      #0 expr  v: string\n"
          "      #1 expr\n"
          "  #3 let  after: dynamic\n");
}

TEST_CASE("a break carries its bindings out of the loop") {
    // `r` is a number at the end of the body, so folding only that back into
    // the header would claim `out` is a number. The `break` leaves from the
    // middle, holding a string.
    const auto inferred = infer(
        "let r = 1;\n"
        "let c = 0;\n"
        "while (c < 3) {\n"
        "  r = \"s\";\n"
        "  if (c === 1) {\n"
        "    break;\n"
        "  }\n"
        "  r = 2;\n"
        "  c = c + 1;\n"
        "}\n"
        "let out = r;\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 let  r: number\n"
          "  #1 let  c: number\n"
          "  #2 while  r: dynamic\n"
          "    body\n"
          "      #0 expr  r: string\n"
          "      #1 if\n"
          "        then\n"
          "          #0 break\n"
          "      #2 expr  r: number\n"
          "      #3 expr\n"
          "  #3 let  out: dynamic\n");
}

TEST_CASE("block-scoped declarations do not leak into the enclosing scope") {
    // A shadowing inner declaration must not narrow the outer binding: that
    // would be the one direction of imprecision the lattice cannot forgive.
    const auto inferred = infer(
        "let x = 1;\n"
        "{\n"
        "  let x = \"s\";\n"
        "}\n"
        "let y = x;\n"
        "let i = 0;\n"
        "for (let i = \"loop\"; false; i = i) {\n"
        "}\n"
        "let j = i;\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 let  x: number\n"
          "  #1 block\n"
          "    #0 let  x: string\n"
          "  #2 let  y: number\n"
          "  #3 let  i: number\n"
          "  #4 for\n"
          "    init\n"
          "      #0 let  i: string\n"
          "  #5 let  j: number\n");
}

TEST_CASE("an env-backed variable is one cell joined across every write") {
    // `n` is written to a number in the declaring function and to a string
    // inside the closure. Flow-sensitivity on it would claim `n` is a number
    // at the `return`, which the closure can falsify at any time.
    const auto inferred = infer(
        "function outer() {\n"
        "  let n = 1;\n"
        "  const g = function () { n = \"s\"; };\n"
        "  return n;\n"
        "}\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func outer() -> dynamic direct-callable\n"
          "  cells  n: dynamic\n"
          // No per-statement entry for `n`: it is not a per-program-point
          // fact, so the declaration does not "change" it.
          "  #0 let\n"
          "  #1 const  g: function\n"
          "  #2 return\n"
          "\n"
          "func outer::<anon0>() -> undefined\n"
          "  #0 expr\n");
}

// ---- shape classes ----------------------------------------------------------

TEST_CASE("structurally identical object literals share one shape class") {
    const auto inferred = infer(
        "function Point(a, b) {\n"
        "  this.x = a;\n"
        "  this.y = b;\n"
        "}\n"
        "const p = { x: 1, y: 2 };\n"
        "const q = { x: 3, y: 4 };\n"
        "const r = { z: 5 };\n"
        "const s = new Point(1, 2);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          // `new Point(...)` is a reference to the name in a position other
          // than callee-of-a-call, so Point is not direct-callable.
          "func Point(a: dynamic, b: dynamic) -> dynamic\n"
          "  #0 expr\n"
          "  #1 expr\n"
          "\n"
          "func main() -> undefined\n"
          // Same ordered property list, same prototype source: one class.
          "  #0 const  p: object#0\n"
          "  #1 const  q: object#0\n"
          "  #2 const  r: object#1\n"
          "  #3 const  s: object#2\n"
          "\n"
          "shapes\n"
          "  #0 {x, y}\n"
          "  #1 {z}\n"
          // Same property names as #0 and a different prototype, so a
          // different class (docs/0008 decision 1).
          "  #2 Point{x, y}\n");
}

TEST_CASE("shape classes are queryable per site") {
    const auto inferred = infer("const p = { x: 1 };\nconst q = { x: 2 };\n");
    const auto& r = *inferred.result;
    REQUIRE(r.shapes.size() == 1);
    CHECK(r.shapes.at(0).constructorName.empty());
    CHECK(r.shapes.at(0).properties == std::vector<std::string>{"x"});

    const auto* first = dynamic_cast<const ast::VarDecl*>(inferred.module->body[0].get());
    const auto* second = dynamic_cast<const ast::VarDecl*>(inferred.module->body[1].get());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(r.shapeClassAt(first->init.get()) == 0u);
    CHECK(r.shapeClassAt(second->init.get()) == 0u);
    CHECK(r.typeAt(first->init.get()) == types::Type::object(0));
    // An expression the analysis never saw answers Dynamic, never an error.
    CHECK(r.typeAt(nullptr) == types::Type::dynamic());
    CHECK(r.shapeClassAt(nullptr) == types::kNoShapeClass);
}

// ---- the call graph ---------------------------------------------------------

TEST_CASE("a name that escapes is not direct-callable") {
    const auto inferred = infer(
        "function direct(a) { return a + 1; }\n"
        "function leaked(a) { return a + 1; }\n"
        "const alias = leaked;\n"
        "direct(1);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func direct(a: number) -> number direct-callable\n"
          "  #0 return\n"
          "\n"
          // Read as a value once, so it keeps the uniform dynamic
          // convention however obvious its body is.
          "func leaked(a: dynamic) -> dynamic\n"
          "  #0 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 const  alias: function#1\n"
          "  #1 expr\n");

    const auto& r = *inferred.result;
    REQUIRE(r.moduleFunctionCount() == 2);
    CHECK(r.isDirectCallable("direct"));
    CHECK_FALSE(r.isDirectCallable("leaked"));
    CHECK_FALSE(r.isDirectCallable("nosuchfunction"));
    CHECK(r.functionIndexOf("leaked") == std::optional<uint32_t>(1));
    CHECK_FALSE(r.functionIndexOf("nosuchfunction").has_value());

    CHECK(r.signatureOf(0).params == std::vector<types::Type>{types::Type::number()});
    CHECK(r.signatureOf(0).returnType == types::Type::number());
    CHECK(r.signatureOf(1).params == std::vector<types::Type>{types::Type::dynamic()});
    CHECK(r.signatureOf(1).returnType == types::Type::dynamic());
    // Out of range is the uniform dynamic convention, not a crash.
    CHECK(r.signatureOf(99).returnType == types::Type::dynamic());
    CHECK_FALSE(r.isDirectCallable(99u));
}

TEST_CASE("a recursive function converges") {
    // The self-call reads a signature that starts at Never and only widens,
    // so the first pass sees `fib(...)` as bottom, the join with `return n`
    // makes the return a number, and the second pass agrees.
    const auto inferred = infer(
        "function fib(n) {\n"
        "  if (n < 2) {\n"
        "    return n;\n"
        "  }\n"
        "  return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "fib(30);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func fib(n: number) -> number direct-callable\n"
          "  #0 if\n"
          "    then\n"
          "      #0 return\n"
          "  #1 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 expr\n");
}

TEST_CASE("mutual recursion converges, and a function with no callers stays at Never") {
    const auto inferred = infer(
        "function isEven(n) {\n"
        "  if (n === 0) { return true; }\n"
        "  return isOdd(n - 1);\n"
        "}\n"
        "function isOdd(n) {\n"
        "  if (n === 0) { return false; }\n"
        "  return isEven(n - 1);\n"
        "}\n"
        "function neverCalled(a) { return a; }\n"
        "isEven(10);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func isEven(n: number) -> bool direct-callable\n"
          "  #0 if\n"
          "    then\n"
          "      #0 return\n"
          "  #1 return\n"
          "\n"
          "func isOdd(n: number) -> bool direct-callable\n"
          "  #0 if\n"
          "    then\n"
          "      #0 return\n"
          "  #1 return\n"
          "\n"
          // No call site anywhere, so nothing has reached the parameter and
          // nothing has reached the return. `Never` is the honest answer:
          // the function is dead, not dynamic. See signatureOf's contract.
          "func neverCalled(a: never) -> never direct-callable\n"
          "  #0 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 expr\n");

    const auto& r = *inferred.result;
    REQUIRE(r.functionIndexOf("neverCalled") == std::optional<uint32_t>(2));
    CHECK(r.isDirectCallable(2u));
    CHECK(r.signatureOf(2).returnType == types::Type::never());
}

TEST_CASE("a parameter joined across call sites of different types is Dynamic") {
    const auto inferred = infer(
        "function id(a) { return a; }\n"
        "id(1);\n"
        "id(\"s\");\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func id(a: dynamic) -> dynamic direct-callable\n"
          "  #0 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 expr\n"
          "  #1 expr\n");
}

TEST_CASE("a missing argument reaches the parameter as undefined") {
    const auto inferred = infer(
        "function two(a, b) { return a; }\n"
        "two(1);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func two(a: number, b: undefined) -> number direct-callable\n"
          "  #0 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 expr\n");
}

TEST_CASE("a shadowing binding stops a name being direct-callable") {
    // `helper` is a parameter of `wrapper`, so a call through that name is
    // not necessarily a call to the module function. The escape test is
    // blunt by design and rules the module function out entirely.
    const auto inferred = infer(
        "function helper(a) { return a; }\n"
        "function wrapper(helper) { return helper(1); }\n"
        "wrapper(1);\n");

    const auto& r = *inferred.result;
    CHECK_FALSE(r.isDirectCallable("helper"));
    CHECK(r.isDirectCallable("wrapper"));
}
