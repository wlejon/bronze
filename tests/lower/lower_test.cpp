#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "il/print.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "types/infer.h"

using namespace bronze;

static std::unique_ptr<ast::Module> parseOnly(std::string_view src, DiagnosticSink& diags,
                                              SourceBuffer& buf) {
    buf = SourceBuffer("test.ts", std::string(src));
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) return nullptr;
    return Parser(std::move(tokens), diags).parseModule("test");
}

// Lowering with NO inference result — the `--no-infer` path of docs/0010
// decision 8. Everything is the uniform dynamic convention, and an
// annotation buys nothing, because nothing is proven for it to agree with.
static std::optional<il::Module> parseAndLower(std::string_view src, DiagnosticSink& diags, SourceBuffer& buf) {
    auto astMod = parseOnly(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    return lower::lowerModule(*astMod, diags);
}

// The real pipeline: inference runs first and lowering consumes the side
// table (docs/0010 decision 1).
static std::optional<il::Module> inferAndLower(std::string_view src, DiagnosticSink& diags,
                                               SourceBuffer& buf) {
    auto astMod = parseOnly(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) return std::nullopt;
    return lower::lowerModule(*astMod, diags, &*inferred);
}

// The same source in both tests below. `a` and `b` carry the SAME
// annotation and get different IL types, which is the whole of docs/0010
// decision 6 in one function: the type comes from the proof, never from the
// annotation.
static constexpr const char* kAnnotatedArithmetic =
    "function add(a: number, b: number): number {\n"
    "  return a + b;\n"
    "}\n"
    "export function calculate(x: number): number {\n"
    "  const doubled = x * 2;\n"
    "  const difference = doubled - 1;\n"
    "  const ratio = difference / 2;\n"
    "  return add(ratio, x);\n"
    "}\n";

TEST_CASE("a proven signature types the IL; the annotation on it does not") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(kAnnotatedArithmetic, diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const std::string printed = il::print(*optMod);
    // `add` is direct-callable and its one call site is `add(ratio, x)`:
    // `ratio` is a proven number, so `a` is f64 — and `x` is a parameter of
    // an EXPORTED function, whose callers are outside this compilation, so
    // `b` is dynamic despite the identical `: number` on it. `a + b` is
    // therefore the dynamic JS `+` (it must be: `b` could be a string), and
    // the return joins to dynamic with it.
    //
    // `calculate` escapes through `export`, so it keeps the uniform dynamic
    // convention whatever its annotations say. Its `x * 2` still unboxes,
    // and that is not the old unsoundness: `*` is ToNumber on both operands
    // in every case (ECMA-262 13.6), unlike `+`.
    CHECK(printed ==
          "module test\n"
          "\n"
          "func add(%0: f64, %1: dynamic) -> dynamic {\n"
          "  b0:\n"
          "    %2: dynamic = box.f64 %0\n"
          "    %3: dynamic = add %2, %1\n"
          "    ret %3\n"
          "}\n"
          "\n"
          "func calculate(%0: dynamic) -> dynamic export {\n"
          "  b0:\n"
          "    %1: f64 = const.f64 2\n"
          "    %2: f64 = unbox.f64 %0\n"
          "    %3: f64 = mul %2, %1\n"
          "    %4: f64 = const.f64 1\n"
          "    %5: f64 = sub %3, %4\n"
          "    %6: f64 = const.f64 2\n"
          "    %7: f64 = div %5, %6\n"
          "    %8: dynamic = call @add(%7, %0)\n"
          "    ret %8\n"
          "}\n");

    // Every discarded annotation is named, and the one the proof agreed
    // with is not mentioned at all — it was free information.
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("annotation 'number' on 'a'") == std::string::npos);
    CHECK(rendered.find("warning: annotation 'number' on 'b' is not provable; ignoring "
                        "(inferred: dynamic)") != std::string::npos);
    CHECK(rendered.find("warning: annotation 'number' on 'add' is not provable; ignoring "
                        "(inferred: dynamic)") != std::string::npos);
    CHECK(rendered.find("warning: annotation 'number' on 'x' is not provable; ignoring "
                        "(inferred: dynamic)") != std::string::npos);
    CHECK(rendered.find("warning: annotation 'number' on 'calculate' is not provable; ignoring "
                        "(inferred: dynamic)") != std::string::npos);
}

TEST_CASE("with no inference, an annotation types nothing at all") {
    // The same source through `--no-infer`. There is no proof for any
    // annotation to agree with, so all four positions are dynamic and every
    // annotation is discarded — including `a`, which the proof accepted
    // above. That is what makes `--no-infer` a bisection seam rather than a
    // second, more trusting compiler.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(kAnnotatedArithmetic, diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func add(%0: dynamic, %1: dynamic) -> dynamic") != std::string::npos);
    CHECK(printed.find("func calculate(%0: dynamic) -> dynamic export") != std::string::npos);

    // ...but discarding them is not WARNED about here. Under `--no-infer`
    // nothing is provable by construction, so the warning would fire on
    // every annotation in the file and say nothing about any of them — only
    // that the switch is on. The suppression is exactly the "no inference
    // result" test, so it cannot reach the mode above, where the same source
    // warns about `b`, `x`, `add` and `calculate` (the test before this one).
    CHECK(diags.render(buf).find("annotation") == std::string::npos);
}

TEST_CASE("--no-infer suppresses annotation warnings but not the annotation error") {
    // The suppression is about a warning that carries no information in that
    // mode. Unreadable annotation text is information about the SOURCE, and
    // `--no-infer` is a bisection seam: it must not accept a file the normal
    // mode rejects.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower("function f(x: Widget) { return x; }\n", diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    CHECK(diags.render(buf).find("unsupported type annotation: Widget") != std::string::npos);
}

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

TEST_CASE("an annotation contradicted by the initialiser is a warning, not a cast") {
    // `let s: number = "abc"` used to emit `unbox.f64` of a boxed string —
    // a coercion the source never wrote, and the live unsoundness docs/0010
    // named. The annotation is now discarded and the binding stays a string.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("let s: number = \"abc\";\nconsole.log(s);\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("unbox.f64") == std::string::npos);
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("warning: annotation 'number' on 's' contradicts inferred string") !=
          std::string::npos);
}

TEST_CASE("a closure's parameter annotations are never provable") {
    // A closure is reached through a function value, so its callers are not
    // a set this compilation can close over (docs/0010 decision 5 excludes
    // it from signature specialization). A PARAMETER of one therefore has no
    // proof at all, and cannot acquire one here.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "const f = function (a: number) { return a + 1; };\nconsole.log(f(1));\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("(%0: dynamic, %1: dynamic) -> dynamic") != std::string::npos);
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("warning: annotation 'number' on 'a' is not provable") !=
          std::string::npos);
}

TEST_CASE("a closure's RETURN annotation is checked against the body") {
    // The proof surface docs/0010 recorded as missing. What a closure
    // returns is a fact about its body, which the analysis already computes;
    // only its parameters are beyond reach. So a correct return annotation
    // on a closure is silent, and a wrong one is a contradiction rather than
    // "not provable".
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "const good = function agrees(): number { return 1; };\n"
        "const bad = function disagrees(): string { return 2; };\n"
        "console.log(good());\nconsole.log(bad());\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string rendered = diags.render(buf);
    // The agreeing one buys nothing: a closure's return is the uniform
    // dynamic convention whatever the body does, and the annotation may not
    // widen that. It is simply not complained about.
    CHECK(rendered.find("on 'agrees'") == std::string::npos);
    CHECK(rendered.find("warning: annotation 'string' on 'disagrees' contradicts inferred "
                        "number") != std::string::npos);
}

TEST_CASE("annotations are read in TypeScript's spellings, not the IL's") {
    // The premise of the policy is that an annotation is an untrusted TS
    // hint, so `string`/`boolean`/`number` have to be readable. bronze's own
    // IL names keep working beside them.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "let a: string = \"x\";\n"
        "let b: str = \"y\";\n"
        "let c: boolean = true;\n"
        "let d: unknown = 1;\n"
        "let e: number = \"nope\";\n"
        "console.log(a);\nconsole.log(b);\nconsole.log(c);\nconsole.log(d);\nconsole.log(e);\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("on 'a'") == std::string::npos);
    CHECK(rendered.find("on 'b'") == std::string::npos);
    CHECK(rendered.find("on 'c'") == std::string::npos);
    // `unknown` is the top type: it claims nothing, so nothing can disagree.
    CHECK(rendered.find("on 'd'") == std::string::npos);
    CHECK(rendered.find("warning: annotation 'number' on 'e' contradicts inferred string") !=
          std::string::npos);
}

TEST_CASE("annotation text bronze cannot read is a hard error, not a silent skip") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower("function f(x: Widget) { return x; }\n", diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("unsupported type annotation: Widget") != std::string::npos);
    // The vocabulary is in the message, so the fix does not need this file.
    CHECK(rendered.find("bronze reads: any, bool, boolean") != std::string::npos);
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

TEST_CASE("deferred construct switch generates diagnostic error") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "switch (1) { case 1: break; }\n",
        diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("unsupported construct: switch statement") != std::string::npos);
}

TEST_CASE("undefined variable reference generates diagnostic error") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "const y = x + 1;\n",
        diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    const std::string rendered = diags.render(buf);
    CHECK(rendered.find("undefined variable: x") != std::string::npos);
}

TEST_CASE("loop-carried bindings are dynamic when nothing proves otherwise") {
    // These tests lower with no inference result, which is exactly the
    // `--no-infer` path (docs/0010 decision 8). A loop header's block
    // parameters have to be an upper bound of every edge into the header,
    // and the back edge is not lowered yet — so with nothing proven the
    // only sound parameter type is `dynamic`. Taking the type of whatever
    // value was live at loop *entry* instead is a claim that the loop
    // cannot change the binding's type, and this loop does: it compiled
    // into an unbox of a string as a double.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let x = 1;\n"
        "let c = true;\n"
        "while (c) { x = \"s\"; c = false; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("b1(%2: dynamic, %3: dynamic):") != std::string::npos);
    // The entry edge boxes into the header rather than the back edge
    // unboxing into it.
    CHECK(printed.find("box.f64 %0") != std::string::npos);
    CHECK(printed.find("unbox.f64") == std::string::npos);
}

TEST_CASE("a module function's return type is settled before any body is lowered") {
    // Mutual recursion: whichever body is lowered first calls a function
    // whose body has not been lowered. If the callee's return type were
    // still being discovered from its first `return`, the call site would
    // read `void` — "returns nothing" — and emit a `ret` of a value that
    // does not exist. With no inference the sound answer is the uniform
    // dynamic convention.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function isEven(n) { if (n === 0) { return true; } return isOdd(n - 1); }\n"
        "function isOdd(n) { if (n === 0) { return false; } return isEven(n - 1); }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func isEven(%0: dynamic) -> dynamic") != std::string::npos);
    CHECK(printed.find("func isOdd(%0: dynamic) -> dynamic") != std::string::npos);
    CHECK(printed.find("4294967295") == std::string::npos);
}

TEST_CASE("a function returning two types returns dynamic, not its first return's type") {
    // First-return-wins made the rest of the returns unbox into it, so
    // `return "s"` read a string pointer as a double.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(c) { if (c) { return 1; } return \"s\"; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("func f(%0: dynamic) -> dynamic") != std::string::npos);
    CHECK(printed.find("unbox.f64") == std::string::npos);
}

TEST_CASE("a provided global resolves to global.get; an unknown free name still errors") {
    // The globals list is closed at COMPILE time (docs/0011 decision 1):
    // `Math` becomes an instruction, and anything not on the list keeps the
    // diagnostic it has always had rather than becoming a runtime miss.
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
    CHECK(diags2.hasErrors());
    CHECK_FALSE(unknown.has_value());
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
