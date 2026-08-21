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

    // Same kind, different identity: the kind survives, the identity does not.
    // This is an "Object with no class".
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
    // `number | undefined` collapses to Dynamic because there are no unions;
    // the narrow case of a binding written once before any use is the flow
    // analysis's job, not the type's. Both are visible here: nothing collapses,
    // because the two writes never reach the same program point.
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
          // The parameter comes from the one call site.
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
          // Same property names as #0 and a different prototype, so a different
          // class.
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

TEST_CASE("an exported function is not direct-callable, however plain its call sites") {
    // A signature is specialized on a join over *every* caller. An export has a
    // caller outside this compilation, so the join is incomplete and the
    // specialization would be a guess. `internal` shows the same body and the
    // same call site do get specialized when nothing exports them.
    const auto inferred = infer(
        "export function shared(x) { return x * 2; }\n"
        "function internal(x) { return x * 2; }\n"
        "shared(4);\n"
        "internal(4);\n");

    CHECK(inferred.dump() ==
          "module test\n"
          "\n"
          "func shared(x: dynamic) -> dynamic\n"
          "  #0 return\n"
          "\n"
          "func internal(x: number) -> number direct-callable\n"
          "  #0 return\n"
          "\n"
          "func main() -> undefined\n"
          "  #0 expr\n"
          "  #1 expr\n");

    const auto& r = *inferred.result;
    CHECK_FALSE(r.isDirectCallable("shared"));
    CHECK(r.isDirectCallable("internal"));
}

// ---- the type of a binding at a merge point ---------------------------------

TEST_CASE("a binding's type at a loop is the join over every edge of the loop") {
    // The query lowering needs to type a loop header's block parameters before
    // it has lowered the back edge. `v` is a number on the entry edge and a
    // string on the back edge; answering "number" here is the miscompile that
    // unboxes a string as a double.
    const auto inferred = infer(
        "let v = 1;\n"
        "let i = 0;\n"
        "let untouched = 7;\n"
        "while (i < 3) {\n"
        "  v = \"s\";\n"
        "  i = i + 1;\n"
        "}\n");

    const ast::Stmt* loop = nullptr;
    for (const auto& s : inferred.module->body) {
        if (dynamic_cast<const ast::WhileStmt*>(s.get()) != nullptr) loop = s.get();
    }
    REQUIRE(loop != nullptr);

    const auto& r = *inferred.result;
    CHECK(r.typeOfBindingAt(loop, "v") == types::Type::dynamic());
    CHECK(r.typeOfBindingAt(loop, "i") == types::Type::number());
    CHECK(r.typeOfBindingAt(loop, "untouched") == types::Type::number());
    // Unproven is Dynamic, never a crash and never a claim: an unknown
    // name, and a merge point the analysis never recorded.
    CHECK(r.typeOfBindingAt(loop, "nosuchname") == types::Type::dynamic());
    CHECK(r.typeOfBindingAt(nullptr, "v") == types::Type::dynamic());
}

TEST_CASE("a binding's type at an if is the join of the two arms") {
    const auto inferred = infer(
        "let v = 1;\n"
        "let n = 0;\n"
        "if (v > 0) {\n"
        "  v = \"s\";\n"
        "  n = 2;\n"
        "} else {\n"
        "  n = 3;\n"
        "}\n");

    const ast::Stmt* branch = nullptr;
    for (const auto& s : inferred.module->body) {
        if (dynamic_cast<const ast::IfStmt*>(s.get()) != nullptr) branch = s.get();
    }
    REQUIRE(branch != nullptr);

    const auto& r = *inferred.result;
    // One arm leaves a string, the other a number.
    CHECK(r.typeOfBindingAt(branch, "v") == types::Type::dynamic());
    // Both arms leave a number, from different assignments.
    CHECK(r.typeOfBindingAt(branch, "n") == types::Type::number());
}

TEST_CASE("the operator result rules hold regardless of operand type") {
    // Inference must agree with lowering about these, or the with-inference
    // and `--no-infer` runs of the oracle suite would disagree: bitwise and
    // shift results are always numbers however dynamic the operands are.
    const auto inferred = infer(
        "let s = \"x\";\n"
        "let a = s & 1;\n"
        "let b = s >>> 1;\n"
        "let c = ~s;\n"
        "let d = s ** 2;\n"
        "let e = typeof s;\n"
        "let f = void s;\n"
        "let g = s instanceof Object;\n"
        "let h = s in s;\n"
        "let i = (s, 1);\n"
        "let j = 1;\n"
        "j &= s;\n");

    const auto dump = inferred.dump();
    CHECK(dump.find("a: number") != std::string::npos);
    CHECK(dump.find("b: number") != std::string::npos);
    CHECK(dump.find("c: number") != std::string::npos);
    CHECK(dump.find("d: number") != std::string::npos);
    CHECK(dump.find("e: string") != std::string::npos);
    CHECK(dump.find("f: undefined") != std::string::npos);
    CHECK(dump.find("g: bool") != std::string::npos);
    CHECK(dump.find("h: bool") != std::string::npos);
    // Comma evaluates the left for effect and takes the RIGHT operand's type.
    CHECK(dump.find("i: number") != std::string::npos);
    // A compound bitwise assignment is the operator, so the binding stays a
    // number even though the right operand is a string.
    CHECK(dump.find("j: number") != std::string::npos);
}

TEST_CASE("an unresolved name is dynamic, and does not poison what surrounds it") {
    // A name nothing declares reaches inference. The only sound answer for it
    // is `dynamic` — what the running environment holds is not a fact this
    // compilation has — and, just as importantly, the code AROUND it stays
    // analysable: a `typeof` guard is an ordinary condition, and a binding
    // assigned from a literal in the same function is still proven a number.
    const auto inferred = infer(
        "function probe() {\n"
        "  const found = __MISSING__;\n"
        "  let n = 0;\n"
        "  if (typeof __MISSING__ !== \"undefined\") {\n"
        "    n = 1;\n"
        "  }\n"
        "  return n;\n"
        "}\n"
        "console.log(probe());\n");

    const ast::FunctionDecl* probe = nullptr;
    for (const auto& s : inferred.module->body) {
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(s.get())) probe = fn;
    }
    REQUIRE(probe != nullptr);

    // The `if` is the merge point: what its join produced is what the code
    // after it believes.
    const ast::Stmt* branch = nullptr;
    for (const auto& s : probe->body) {
        if (dynamic_cast<const ast::IfStmt*>(s.get()) != nullptr) branch = s.get();
    }
    REQUIRE(branch != nullptr);

    const auto& r = *inferred.result;
    // The unresolved name itself: the only sound answer.
    CHECK(r.typeOfBindingAt(branch, "found") == types::Type::dynamic());
    // And the join still happened over BOTH arms — the guard did not make the
    // then-arm unreachable, and the surrounding binding keeps its element
    // type rather than degrading to dynamic because something near it was
    // unresolved.
    CHECK(r.typeOfBindingAt(branch, "n") == types::Type::number());
    // Inference reports nothing at all for the unresolved name; the warning
    // is lowering's, and the guard is an ordinary condition.
    CHECK(inferred.diags.all().empty());
}

TEST_CASE("a generator's signature is the generator object, not what its body returns") {
    // ECMA-262 27.5.1.2: calling a generator function runs NONE of the body.
    // It builds a generator object and returns that, so the `return` operand
    // is the `value` of the final result (27.5.3.2) and says nothing about
    // what the call evaluates to. A number is the operand that makes reading
    // it visible: a caller told to expect one unboxes the object it actually
    // gets. The plain function beside it is the control — the rule is about
    // generators, and must not have cost every other function its return
    // proof.
    const auto inferred = infer(
        "function* gen(x) { yield x; return 2; }\n"
        "function plain(x) { return 2; }\n"
        "const g = gen(1);\n"
        "const p = plain(1);\n"
        "console.log(g, p);\n");

    const auto genIndex = inferred.result->functionIndexOf("gen");
    const auto plainIndex = inferred.result->functionIndexOf("plain");
    REQUIRE(genIndex.has_value());
    REQUIRE(plainIndex.has_value());
    CHECK(inferred.result->signatureOf(*genIndex).returnType == types::Type::dynamic());
    CHECK(inferred.result->signatureOf(*plainIndex).returnType == types::Type::number());

    // The PARAMETERS are a different fact and survive: the factory really is
    // a function of them, and it really is called with them. Throwing them
    // away with the return would cost the specialization for no reason.
    CHECK(inferred.result->isDirectCallable(*genIndex));
    REQUIRE(inferred.result->signatureOf(*genIndex).params.size() == 1);
    CHECK(inferred.result->signatureOf(*genIndex).params[0] == types::Type::number());

    // And the call site believes it. This is the assertion that was silently
    // false: `g` was `number`, so lowering unboxed a generator object.
    CHECK(inferred.dump().find("func gen(x: number) -> dynamic") != std::string::npos);
    CHECK(inferred.dump().find("func plain(x: number) -> number") != std::string::npos);
}

TEST_CASE("a generator function expression reports the object as its closure return") {
    // The same rule reached through the other path. `closureReturnAt` is a
    // closure's whole proof surface, and it is what decides whether a
    // `: number` annotation on the generator is endorsed or reported
    // unprovable — endorsing it would be endorsing a wrong description of the
    // call.
    const auto inferred = infer(
        "const gen = function* () { return 2; };\n"
        "const plain = function () { return 2; };\n"
        "console.log(gen, plain);\n");

    const ast::FunctionExpr* genExpr = nullptr;
    const ast::FunctionExpr* plainExpr = nullptr;
    for (const auto& s : inferred.module->body) {
        const auto* decl = dynamic_cast<const ast::VarDecl*>(s.get());
        if (decl == nullptr || decl->init == nullptr) continue;
        const auto* fn = dynamic_cast<const ast::FunctionExpr*>(decl->init.get());
        if (fn == nullptr) continue;
        (fn->isGenerator ? genExpr : plainExpr) = fn;
    }
    REQUIRE(genExpr != nullptr);
    REQUIRE(plainExpr != nullptr);
    CHECK(inferred.result->closureReturnAt(genExpr) == types::Type::dynamic());
    CHECK(inferred.result->closureReturnAt(plainExpr) == types::Type::number());
}

// ---- the builtin-identity proofs --------------------------------------------

TEST_CASE("the TypedArray lattice element carries its element kind through join") {
    using types::Type;
    using types::TypedArrayElem;
    const Type f64 = Type::typedArray(TypedArrayElem::Float64);
    const Type f32 = Type::typedArray(TypedArrayElem::Float32);
    CHECK(join(f64, f64) == f64);
    CHECK(join(f64, f64).typedArrayElemRaw() ==
          static_cast<uint32_t>(TypedArrayElem::Float64));
    // Different element kinds keep the kind and lose the element — the same
    // rule Object identities follow — because a consumer that knows only
    // "some typed array" still cannot pick an access width.
    CHECK(join(f64, f32) == Type::typedArray());
    CHECK(join(f64, f32).typedArrayElemRaw() == types::kNoTypedArrayElem);
    CHECK(join(f64, Type::object()) == Type::dynamic());
    CHECK(f64.str() == "typedarray:f64");
    CHECK(f32.str() == "typedarray:f32");
}

TEST_CASE("new Float64Array on the unshadowed builtin is a proven view") {
    const auto inferred = infer("const a = new Float64Array(4);\n");
    const auto* decl = dynamic_cast<const ast::VarDecl*>(inferred.module->body[0].get());
    REQUIRE(decl != nullptr);
    CHECK(inferred.result->typeAt(decl->init.get()) ==
          types::Type::typedArray(types::TypedArrayElem::Float64));
}

TEST_CASE("a shadowed Float64Array is not the builtin and proves nothing") {
    const auto inferred = infer(
        "const Float64Array = function (n) { this.len = n; };\n"
        "const a = new Float64Array(4);\n");
    const auto* decl = dynamic_cast<const ast::VarDecl*>(inferred.module->body[1].get());
    REQUIRE(decl != nullptr);
    CHECK_FALSE(inferred.result->typeAt(decl->init.get())
                    .is(types::TypeKind::TypedArray));
}

TEST_CASE("a pristine Math call is typed Number and its site is recorded") {
    const auto inferred = infer(
        "const x = 2.0;\n"
        "const y = Math.sqrt(x);\n");
    const auto* decl = dynamic_cast<const ast::VarDecl*>(inferred.module->body[1].get());
    REQUIRE(decl != nullptr);
    CHECK(inferred.result->typeAt(decl->init.get()) == types::Type::number());
    CHECK(inferred.result->pristineMathCalls.count(decl->init.get()) == 1);
}

TEST_CASE("one write through Math taints the whole module's pristine proof") {
    const auto inferred = infer(
        "Math.extra = 1;\n"
        "const y = Math.sqrt(4.0);\n");
    CHECK(inferred.result->pristineMathCalls.empty());
    const auto* decl = dynamic_cast<const ast::VarDecl*>(inferred.module->body[1].get());
    REQUIRE(decl != nullptr);
    CHECK(inferred.result->typeAt(decl->init.get()) == types::Type::dynamic());
}

TEST_CASE("a bare mention of Math aliases it and forfeits the proof") {
    const auto inferred = infer(
        "const m = Math;\n"
        "const y = Math.sqrt(4.0);\n");
    CHECK(inferred.result->pristineMathCalls.empty());
}

TEST_CASE("a binding named Math shadows the builtin at its site only") {
    const auto inferred = infer(
        "function shadowed() {\n"
        "  const Math = { sqrt: function (n) { return n; } };\n"
        "  return Math.sqrt(9);\n"
        "}\n"
        "const y = Math.sqrt(4.0);\n");
    // The module-level call keeps the proof; the shadowed one is not recorded.
    CHECK(inferred.result->pristineMathCalls.size() == 1);
    const auto* decl = dynamic_cast<const ast::VarDecl*>(inferred.module->body[1].get());
    REQUIRE(decl != nullptr);
    CHECK(inferred.result->pristineMathCalls.count(decl->init.get()) == 1);
}

TEST_CASE("an update on a possibly-BigInt binding does not sharpen it to number") {
    // 13.4.4 stores ToNumeric of the old value, and a BigInt survives that —
    // `let c = 1n; c++` leaves a BigInt. Claiming `number` here once licensed
    // an f64 unbox of a BigInt box at every later read of `c`.
    const auto inferred = infer(
        "function big() { let c = 9007199254740993n; c++; return c; }\n"
        "function small() { let c = 5; c++; return c; }\n"
        "console.log(big(), small());\n");
    const auto& r = *inferred.result;
    REQUIRE(r.isDirectCallable("big"));
    REQUIRE(r.isDirectCallable("small"));
    CHECK(r.signatureOf(*r.functionIndexOf("big")).returnType == types::Type::dynamic());
    CHECK(r.signatureOf(*r.functionIndexOf("small")).returnType == types::Type::number());
}

// ---- the field-type write audit ---------------------------------------------
//
// A harvested field type is a claim about every write in the program, and
// nothing a JS program prints can distinguish "proven, so read raw" from
// "refused, so read boxed" — both answer the same value. These check the
// decision itself: which names survived, why the others did not, and which read
// sites the licence actually reached.
//
// One counting note the numbers below only make sense with: a STORE target is a
// member expression too, so `v.x = n` is one of the sites these counters see and
// one of the nodes the licence lands on. That is harmless — lowering only ever
// consults the set from a read position, and a compound assignment's read of its
// own target is the same node — but it means a source with two reads and one
// write of a proven field reports three sites, not two.

TEST_CASE("a field written only with numbers licenses a raw read") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.refusals.empty());
    CHECK(r.fieldAudit.globalRefusals.empty());
    // Both reads of `v.x` inside `read`, plus the store target in `make`.
    CHECK(r.provenFieldReads.size() == 3);
    // The fourth site is `this.x = 0` in the constructor, whose base is a
    // receiver and not an object this compilation watched being made.
    CHECK(r.fieldAudit.numberFieldReads == 4);
    CHECK(r.fieldAudit.refusedNotBuiltHere == 1);
    CHECK(r.fieldAudit.refusedByAudit == 0);
}

TEST_CASE("one string write anywhere in the program refuses the name") {
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "const v = make(1);\n"
        "v.x = 'hi';\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.refusals.count("written as string") == 1);
    CHECK(r.provenFieldReads.empty());
    // The sites were still a Number harvest; the audit is what stood them down —
    // all but the constructor's `this.x`, which never got that far.
    CHECK(r.fieldAudit.numberFieldReads == 5);
    CHECK(r.fieldAudit.refusedByAudit == 4);
    CHECK(r.fieldAudit.refusedNotBuiltHere == 1);
}

TEST_CASE("the audit's unit is the property name, not the class") {
    // `W` never writes a string, but a write to some OTHER object's `x` is a
    // write a dynamic receiver could have aimed at a `W`. Scoping the audit to
    // the class that declared the field would certify this and be wrong.
    const auto inferred = infer(
        "class W { constructor() { this.x = 0; } }\n"
        "function make(n) { const w = new W(); w.x = n; return w; }\n"
        "function read(w) { let s = w.x; for (let i = 0; i < 2; i++) s = w.x; return s; }\n"
        "const other = { x: 1 };\n"
        "other.x = 'not a number';\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.refusals.count("written as string") == 1);
    CHECK(r.provenFieldReads.empty());
}

TEST_CASE("one computed write stands every name in the module down") {
    // The key is not a literal, so the write names no property and the audit
    // cannot bound what it touched. This is the refutation that costs three.js
    // its field types, so it is worth pinning what it does and does not do:
    // every name is refused, and the refusal is recorded as GLOBAL, not as a
    // fact about `x`.
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "function poke(o, k, val) { o[k] = val; }\n"
        "const v = make(1);\n"
        "poke(v, 'x', 'hi');\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.namesLocallyClean == 1);  // `x` itself is otherwise clean
    CHECK(r.fieldAudit.globalRefusals.size() == 1);
    CHECK(r.provenFieldReads.empty());
}

TEST_CASE("a computed write with a number key cannot name a non-numeric field") {
    // `o[i] = s` can only produce a numeric property key, so it refutes nothing
    // about `x`. Without this the audit loses every name to any indexed write.
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "function fill(o) { for (let i = 0; i < 4; i++) o[i] = 'text'; }\n"
        "const v = make(1);\n"
        "fill([]);\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.globalRefusals.empty());
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.provenFieldReads.size() == 3);
}

TEST_CASE("a `this` receiver is refused even when the name is clean") {
    // A method's receiver is whatever the call passed. The name survives the
    // audit — nothing writes a string — but the base was not watched being
    // made, so the field's PRESENCE is not proven and the read stays boxed.
    const auto inferred = infer(
        "class V {\n"
        "  constructor() { this.x = 0; }\n"
        "  read() { let s = this.x; for (let i = 0; i < 2; i++) s = this.x; return s; }\n"
        "}\n"
        "console.log(new V().read());\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.provenFieldReads.empty());
    // Both reads, and the constructor's own store target.
    CHECK(r.fieldAudit.refusedNotBuiltHere == 3);
    CHECK(r.fieldAudit.refusedByAudit == 0);
}

TEST_CASE("an accessor over the name refuses the field, not the write") {
    // `get x()` means the read runs code. The audit sees no bad write, so the
    // refusal has to come from the class layout's accessor list.
    const auto inferred = infer(
        "class V {\n"
        "  constructor() { this._x = 0; }\n"
        "  get x() { return this._x; }\n"
        "}\n"
        "function make(n) { const v = new V(); v._x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    // The two `v.x` reads are refused a rung EARLIER than the audit: an accessor
    // name is not an instance field, so the class harvest never types the read
    // `number` and there is no Number claim to refuse. They are not in the
    // population at all — the three sites counted are the ones naming `_x`,
    // which IS a field and IS clean, and only `make`'s store target has a base
    // this compilation watched being made.
    CHECK(r.fieldAudit.namesClean == 1);
    CHECK(r.fieldAudit.numberFieldReads == 3);
    CHECK(r.fieldAudit.refusedNotBuiltHere == 2);
    CHECK(r.provenFieldReads.size() == 1);
    CHECK(r.fieldAudit.refusedByAudit == 0);
}

TEST_CASE("an accessor anywhere in the program refuses the name globally") {
    // `x` is a data field of `Base` and every write of it is a number, so the
    // write scan alone would certify it. A subclass declaring `get x`/`set x`
    // is what refuses it, and it refuses the NAME — the audit cannot know which
    // objects reach a `d.x` store, so a getter declared for the name anywhere
    // makes every claim about a slot of that name a claim about a call.
    //
    // Two mechanisms answer this, deliberately: the audit refuses the name, and
    // `fieldValueCandidate` refuses the read against the class's accessor list
    // walked to the root of the `extends` chain. The audit gets there first,
    // which is why the class-layout counter stays zero here.
    const auto inferred = infer(
        "class Base { constructor() { this.x = 0; } }\n"
        "class Derived extends Base {\n"
        "  get x() { return 5; }\n"
        "  set x(v) { this._v = v; }\n"
        "}\n"
        "function make(n) { const d = new Derived(); d.x = n; return d; }\n"
        "function read(d) { let s = d.x; for (let i = 0; i < 2; i++) s = d.x; return s; }\n"
        "console.log(read(make(1)));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 0);
    CHECK(r.fieldAudit.refusals.count("declared as a class accessor") == 1);
    CHECK(r.fieldAudit.refusedByClass == 0);
    CHECK(r.provenFieldReads.empty());
}

TEST_CASE("a numeric compound assignment preserves the field's proof") {
    // `+=` on a number-typed read is a number, and so are `++`, `*=` and `-`.
    // Refusing them would cost every accumulator field in a math library.
    const auto inferred = infer(
        "class V { constructor() { this.x = 0; } }\n"
        "function make(n) { const v = new V(); v.x = n; return v; }\n"
        "function read(v) { let s = v.x; for (let i = 0; i < 2; i++) s = v.x; return s; }\n"
        "const v = make(1);\n"
        "v.x += 5;\n"
        "v.x++;\n"
        "v.x *= 2;\n"
        "console.log(read(v));\n");
    const auto& r = *inferred.result;
    CHECK(r.fieldAudit.namesClean == 1);
    // Two reads, `make`'s store, and the three compound targets.
    CHECK(r.provenFieldReads.size() == 6);
}
