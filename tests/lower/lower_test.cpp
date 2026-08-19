// The module skeleton and the expression and statement forms that do not have a
// seam of their own yet: literals, property access and calls, if/switch,
// for-of, classes and `super`, binding patterns, and the operator families. The
// units named for one seam each — inference, resolution, scopes, jumps, try —
// live in their own files.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

TEST_CASE("numeric comparisons <, >, ==") {
    // Unannotated, and proven numeric by the call site instead — which is
    // the point: the comparisons lower to native `cmp.*` because inference
    // proved the operands, not because anyone wrote `: number`.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function compare(a, b) {\n"
        "  const lt = a < b;\n"
        "  const gt = a > b;\n"
        "  const eq = a == b;\n"
        "  const seq = a === b;\n"
        "  return lt;\n"
        "}\n"
        "console.log(compare(1, 2));\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed.find(
              "func compare(%0: f64, %1: f64) -> bool {\n"
              "  b0:\n"
              "    %2: bool = cmp.lt %0, %1\n"
              "    %3: bool = cmp.gt %0, %1\n"
              "    %4: bool = cmp.eq %0, %1\n"
              "    %5: bool = cmp.eq %0, %1\n"
              "    ret %2\n"
              "}\n") != std::string::npos);
}

TEST_CASE("<= and >= are ordered compares, not negations of > and <") {
    // `a <= b` used to be emitted as `!(a > b)` — a cmp.gt, a const.bool false
    // and a cmp.eq. The identity needs a total order and NaN does not give one:
    // ECMA-262 13.10 folds IsLessThan's *undefined* (step 4.c, a NaN operand)
    // to false, while the negation maps it to true. cmp.le and cmp.ge are the
    // ordered compares, which answer false for NaN the way cmp.lt already did.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function compare(a, b) {\n"
        "  const le = a <= b;\n"
        "  const ge = a >= b;\n"
        "  return le;\n"
        "}\n"
        "console.log(compare(1, 2));\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed.find(
              "func compare(%0: f64, %1: f64) -> bool {\n"
              "  b0:\n"
              "    %2: bool = cmp.le %0, %1\n"
              "    %3: bool = cmp.ge %0, %1\n"
              "    ret %2\n"
              "}\n") != std::string::npos);
    // The shape of the old lowering, named so it cannot come back: a compare
    // followed by a comparison against `false` is the negation this replaced.
    CHECK(printed.find("const.bool") == std::string::npos);
}

TEST_CASE("an unproven relational operand takes the runtime algorithm") {
    // A `dynamic` operand may be a STRING, and ECMA-262 13.10.1 step 3 compares
    // two of those by code unit without converting anything. Only a proof that
    // neither side is boxed licenses the machine compare; sending a boxed
    // operand down the f64 path is what made `"a" < "b"` a comparison of two
    // NaNs.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function order(a, b) {\n"
        "  return a < b;\n"
        "}\n"
        "console.log(order('a', 'b'));\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("%2: bool = rel.lt %0, %1") != std::string::npos);
    CHECK(printed.find("cmp.lt") == std::string::npos);
    // No unbox in front of it: ToNumber is step 4, the else-branch, and
    // reaching for it before step 3 is the defect.
    CHECK(printed.find("unbox") == std::string::npos);
}

TEST_CASE("all four relational operators reach the runtime when unproven") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(a, b) { return [a < b, a > b, a <= b, a >= b]; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    for (const char* op : {"rel.lt", "rel.gt", "rel.le", "rel.ge"}) {
        CHECK(printed.find(op) != std::string::npos);
    }
}

TEST_CASE("top-level statements lowered to main") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const x = 10;\n"
        "const y = 20;\n"
        "const z = x + y;\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed ==
          "module test\n"
          "\n"
          "func main() -> void {\n"
          "  b0:\n"
          "    %0: f64 = const.f64 10\n"
          "    %1: f64 = const.f64 20\n"
          "    %2: f64 = add %0, %1\n"
          "    ret\n"
          "}\n");
}

TEST_CASE("string literal lowered to box.str") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const s = \"hello\";\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("%0: dynamic = box.str") != std::string::npos);
}

TEST_CASE("lowering dynamic types, property access, array indexing, and dynamic calls") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "export function testDynamic(obj: dynamic, fn: dynamic): dynamic {\n"
        "  obj.prop = 42;\n"
        "  const val = obj.prop;\n"
        "  const elem = obj[0];\n"
        "  return fn(val, elem);\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    CHECK(printed.find("prop.set %0") != std::string::npos);
    CHECK(printed.find("prop.get %0") != std::string::npos);
    CHECK(printed.find("call.dynamic %1") != std::string::npos);
}

TEST_CASE("IfStmt lowers to branch and join blocks") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function check(x: number): number {\n"
        "  if (x > 0) { return x; } else { return 0; }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("br %") != std::string::npos);
}

TEST_CASE("comparison != lowers to cmp.ne") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const ne = 1 != 2;\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("cmp.ne") != std::string::npos);
}

TEST_CASE("switch lowers to a strict-equality test chain") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "switch (1) { case 1: break; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // ECMA-262 14.12.10 matches with IsStrictlyEqual, never with the loose
    // rules, so a `cmp.eq` here would be a wrong answer and not a style choice.
    CHECK(printed.find("strict.eq") != std::string::npos);
    CHECK(printed.find("br %") != std::string::npos);
}

TEST_CASE("for-of opens an iterator and closes it when the body throws") {
    // The loop is a cursor, not an index and a length — an iterator is opened
    // once, stepped, and read. And the body runs under a handler whose only job
    // is to close the iterator and re-raise, so a `throw` from inside the loop
    // still reaches the iterator's `return` method.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const a = [1];\n"
        "let s = 0;\n"
        "for (const x of a) { s = s + x; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("iter.open") != std::string::npos);
    CHECK(printed.find("iter.step") != std::string::npos);
    CHECK(printed.find("iter.value") != std::string::npos);
    // Suppressed, because a throw is already on its way out and 7.4.9 step 6
    // keeps that one rather than anything `return` might raise.
    CHECK(printed.find("iter.close %4, suppress") != std::string::npos);
    CHECK(printed.find("exc.take") != std::string::npos);
    // No index is threaded through the loop's blocks any more: the record
    // holds the cursor, so the only block parameters are source bindings.
    CHECK(printed.find("iter.advance") == std::string::npos);
}

TEST_CASE("a class lowers to a constructor, a prototype and property writes") {
    // No new runtime concept: the class IS the constructor function, its
    // methods are stored on `.prototype`, and a static is stored on the
    // function itself.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "class Q { constructor() { this.v = 1; } get() { return this.v; } }\n"
        "class P extends Q {\n"
        "  constructor() { super(); }\n"
        "  get() { return super.get(); }\n"
        "  static make() { return 1; }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func Q(%0: dynamic, %1: dynamic)") != std::string::npos);
    CHECK(printed.find("func Q.get(%0: dynamic, %1: dynamic)") != std::string::npos);
    CHECK(printed.find("func P.make(%0: dynamic)") != std::string::npos);
    // `extends` runs before any method is stored: the prototype object it
    // installs is the one they have to land on. Asserted as an ORDER rather
    // than against a value id — the id shifts whenever anything is emitted
    // ahead of the class, and what the test is about is which object the
    // methods reach.
    const size_t extend = printed.find("class.extend");
    REQUIRE(extend != std::string::npos);
    const size_t protoRead = printed.find("prop.get", extend);
    REQUIRE(protoRead != std::string::npos);
    // The key read is "prototype", and the read is on the same line: this is
    // the object `class.extend` has just replaced, not some other property.
    // The index is looked up, not spelled — see `keyIndex`.
    const std::string protoKey =
        ", " + std::to_string(bronze::lower_test::keyIndex(*optMod, "prototype")) + ", ";
    const size_t lineEnd = printed.find('\n', protoRead);
    CHECK(printed.find(protoKey, protoRead) < lineEnd);
    // The methods land on it, so they come after.
    CHECK(printed.find("method.def", protoRead) != std::string::npos);
    // And the base class's own prototype read happened earlier, under its own
    // declaration, which is what makes this one P's rather than Q's.
    CHECK(printed.find("prop.get") < extend);
}

TEST_CASE("super.m() calls the parent's method with the current receiver") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "class Q { constructor() { this.v = 1; } get() { return this.v; } }\n"
        "class P extends Q { constructor() { super(); } get() { return super.get(); } }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    const size_t body = printed.find("func P.get(");
    REQUIRE(body != std::string::npos);
    // Two reads (the parent, then its prototype, then the method) and a
    // dynamic call whose receiver is %1 — the CURRENT `this`, not the
    // parent prototype the function came from. Reading `this.get` instead
    // would find the override and recurse forever.
    CHECK(printed.find("call.dynamic %4, %1", body) != std::string::npos);
}

TEST_CASE("a method with no `this` in it still gets a receiver when it uses super") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "class Q { constructor() { this.v = 1; } tag() { return \"q\"; } }\n"
        "class P extends Q { constructor() { super(); } tag() { return super.tag(); } }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func P.tag(%0: dynamic, %1: dynamic)") != std::string::npos);
}

TEST_CASE("a destructuring assignment reads the whole right side before writing") {
    // `[a, b] = [b, a]` is a swap only if this holds. The array is built first,
    // and every `iter.at` reads THAT array — so no target write can be seen by
    // a later read.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("let a = 1;\nlet b = 2;\n[a, b] = [b, a];\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // The source is checked once, up front, so a later element read cannot
    // blame for-of for a destructuring failure.
    CHECK(printed.find("pattern.check %2, array") != std::string::npos);
    // One iterator over that array, stepped once per element, and closed at
    // the end because the pattern stopped before the source was exhausted
    // (ECMA-262 8.6.2 step 5). Every read names the SAME record, which is
    // what makes the swap a swap.
    CHECK(printed.find("iter.open %5") != std::string::npos);
    CHECK(printed.find("%8: bool = iter.step %6") != std::string::npos);
    CHECK(printed.find("%7: dynamic = iter.value %6") != std::string::npos);
    CHECK(printed.find("%10: bool = iter.step %6") != std::string::npos);
    CHECK(printed.find("%9: dynamic = iter.value %6") != std::string::npos);
    CHECK(printed.find("iter.close %6, abrupt") != std::string::npos);
}

TEST_CASE("object rest into a property reference lowers to object.rest and prop.set") {
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    const auto optMod =
        parseAndLower("const o = {};\n({ ...o.rest } = { a: 1, b: 2 });\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("pattern.check") != std::string::npos);
    CHECK(printed.find("object.rest") != std::string::npos);
    CHECK(printed.find("prop.set") != std::string::npos);
}

TEST_CASE("a default parameter is a branch, and only undefined takes it") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function f(a, b = a * 2) { return b; }\nf(1);\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // A strict comparison against undefined, not a truthiness test: `null`, `0`
    // and `""` are arguments that were passed.
    CHECK(printed.find("const.undefined") != std::string::npos);
    CHECK(printed.find("strict.eq") != std::string::npos);
    // A BRANCH, not a select — the default's own code runs only when it
    // fires, so its side effects fire with it.
    CHECK(printed.find("br ") != std::string::npos);
    // The default sees the parameters to its left: `a * 2` reads `a`.
    CHECK(printed.find("mul") != std::string::npos);
}

TEST_CASE("a rest parameter is not an argument the caller passes") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function count(first, ...rest) { return rest; }\ncount(1, 2, 3);\n",
                      diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // Two IL parameters, because the rest one is still a value the body reads —
    // but the call site builds it rather than passing three arguments, which is
    // what keeps the arity fixed.
    CHECK(printed.find("func count(%0: dynamic, %1: dynamic)") != std::string::npos);
    // The leftovers are appended into an array that starts EMPTY: `create.array
    // n` makes n undefined elements, so a non-zero length here would leave the
    // rest array with two undefineds in front of what the call passed.
    CHECK(printed.find("create.array 0") != std::string::npos);
    CHECK(printed.find("array.append") != std::string::npos);
}

TEST_CASE("a spread call passes one array, and a spread literal appends") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function g(x, y) { return x; }\nconst a = [1, 2];\n"
                      "const b = [0, ...a, 3];\nconst f = g;\nf(...a);\n",
                      diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("array.spread") != std::string::npos);
    CHECK(printed.find("call.dynamic.spread") != std::string::npos);
}

TEST_CASE("a bitwise operator is int32 inside and f64 outside") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("const a = 5 & 3;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // Each operand converts once, and the RESULT is the JS number the int32
    // denotes — an i32 escaping the operator would be a type inference has no
    // element for.
    CHECK(printed.find(": i32 = to.int32") != std::string::npos);
    CHECK(printed.find(": f64 = and") != std::string::npos);
}

TEST_CASE("`~x` is a xor against -1, not an op of its own") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("const a = ~5;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("const.i32 -1") != std::string::npos);
    CHECK(printed.find(": f64 = xor") != std::string::npos);
}

TEST_CASE("numeric truthiness is num.truthy, not cmp.ne against zero") {
    // The two differ at exactly one value: `if (NaN)` is false while `NaN !==
    // NaN` is true, so one op cannot answer both.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "function f(n: number): number { if (n) { return 1; } return 0; }\n"
        "f(1);\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("num.truthy") != std::string::npos);
}

TEST_CASE("loose equality on unproven operands lowers to the runtime algorithm") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("const a = 1 == \"1\";\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("loose.eq") != std::string::npos);
}

TEST_CASE("loose equality on two proven numbers is the same compare as strict") {
    // Not an optimization on top of IsLooselyEqual — it is that algorithm's
    // first step, and it is what makes `==` free in typed code.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("const a = 1 == 2;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("cmp.eq") != std::string::npos);
    CHECK(printed.find("loose.eq") == std::string::npos);
}

// ---- 10.2.9 and 10.2.10, as far as lowering carries them --------------------
//
// The two own properties of a function object are decided here and passed to
// the runtime as numbers: `nameKeyIndex` into the module's key table, and
// `requiredArgs`, which IS 15.1.5 ExpectedArgumentCount and therefore is the
// `length`. What a program sees is pinned by `cases/function_name_length`; what
// this holds is the part a passing oracle case could not distinguish — that
// `length` is a DIFFERENT count from the arity a call is padded to, and that
// the name comes from the surrounding syntax rather than from the IL symbol.

TEST_CASE("a lowered function carries its spec name and its ExpectedArgumentCount") {
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    const auto optMod = parseAndLower(
        "function decl(a, b = 1, ...rest) {}\n"
        "const bound = function () {};\n"
        "const holder = {};\n"
        "holder.slot = function () {};\n"
        "const obj = { key: () => {}, get g() { return 1; } };\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    // Keyed by the IL symbol, which is the thing that is NOT the name.
    auto find = [&](const char* ilName) -> const il::Function* {
        const auto it =
            std::find_if(optMod->functions.begin(), optMod->functions.end(),
                         [&](const il::Function& f) { return f.name == ilName; });
        return it == optMod->functions.end() ? nullptr : &*it;
    };
    auto jsName = [&](const il::Function* f) -> std::string {
        if (!f || f->nameKeyIndex >= optMod->keyConstants.size()) return "<none>";
        return optMod->keyConstants[f->nameKeyIndex];
    };

    const il::Function* decl = find("decl");
    REQUIRE(decl != nullptr);
    CHECK(jsName(decl) == "decl");
    // Three parameters, a `length` of 1, and a padding arity of 2: the two
    // counts disagree here, which is the whole reason they are two numbers.
    CHECK(decl->requiredArgs == 1);
    CHECK(decl->adaptArity() == 2);
    CHECK(decl->callerParamCount() == 2);

    // 8.6.2 NamedEvaluation: an anonymous function expression in a binding's
    // initializer takes the binding's name, even though the IL called it
    // something else entirely.
    const il::Function* bound = find("__anon_fn_1");
    REQUIRE(bound != nullptr);
    CHECK(jsName(bound) == "bound");
    CHECK(bound->requiredArgs == 0);

    // A member expression is NOT one of 8.6.2's positions, so this one is
    // genuinely anonymous — the empty string, and not the property it lands on.
    const il::Function* slot = find("__anon_fn_2");
    REQUIRE(slot != nullptr);
    CHECK(jsName(slot) == "");

    // 13.2.5.5 PropertyDefinitionEvaluation, and 10.2.9's third argument: the
    // accessor's name carries the prefix, and it is applied to the KEY.
    const il::Function* arrow = find("__anon_fn_3");
    REQUIRE(arrow != nullptr);
    CHECK(jsName(arrow) == "key");
    const il::Function* getter = find("get g");
    REQUIRE(getter != nullptr);
    CHECK(jsName(getter) == "get g");
    CHECK(getter->requiredArgs == 0);
}

// 13.2.8.6 spells a template substitution as ToString, and 13.15.3 spells `+`
// as ToPrimitive-then-decide. The two are not the same conversion: ToString
// asks ToPrimitive for hint "string" and `+` asks for no hint at all, which
// reverses the order `valueOf` and `toString` are tried in. So an object that
// defines both gives `${o}` and `'' + o` different answers, and lowering a
// substitution as a concatenation alone would have silently answered `valueOf`'s
// for both.
//
// The empty leading piece stays: without it `${a}${b}` would be `a + b`, which
// for two numbers is addition rather than concatenation.
TEST_CASE("a template substitution lowers to to.string, not to a bare concatenation") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("const o = {};\nconst s = `x${o}y`;\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("= to.string ") != std::string::npos);
    // One per substitution, and the pieces around it are still adds.
    size_t occurrences = 0;
    for (size_t at = printed.find("to.string"); at != std::string::npos;
         at = printed.find("to.string", at + 1)) {
        ++occurrences;
    }
    CHECK(occurrences == 1);
    CHECK(printed.find("= add ") != std::string::npos);
}

TEST_CASE("logical assignment operators lower to conditional branching") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let x = 1;\nx ||= 2;\nx &&= 3;\nx ??= 4;\n"
        "const o = { a: 1 };\no.a ||= 2;\no.a &&= 3;\no.a ??= 4;\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("br ") != std::string::npos);
    CHECK(printed.find("is.nullish") != std::string::npos);
    CHECK(printed.find("prop.set") != std::string::npos);
}

TEST_CASE("for-in inside try-catch lowers cleanly") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("function f() { try { for (var k in {}) {} } catch (e) {} }", diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
}

TEST_CASE("class extends MemberExpression and dynamic superclass lower to class.extend") {
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    const auto optMod = parseAndLower(
        "const THREE = { EventDispatcher: function () {} };\n"
        "class EditorControls extends THREE.EventDispatcher {}\n"
        "const x = { y: { Z: function () {} } };\n"
        "class A extends x.y.Z {}\n"
        "function getBase() { return function () {}; }\n"
        "class Dyn extends getBase() {}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // Each extends emits class.extend with the constructor and base
    size_t extendCount = 0;
    for (size_t pos = printed.find("class.extend"); pos != std::string::npos;
         pos = printed.find("class.extend", pos + 1)) {
        ++extendCount;
    }
    CHECK(extendCount >= 3);
}

