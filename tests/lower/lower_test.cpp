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
    // rules, so a `cmp.eq` here would be a wrong answer and not a style
    // choice (docs/0018 decision 4).
    CHECK(printed.find("strict.eq") != std::string::npos);
    CHECK(printed.find("br %") != std::string::npos);
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

TEST_CASE("for-of opens an iterator and closes it when the body throws") {
    // docs/0021 decision 2: the loop is a cursor, not an index and a length
    // — an iterator is opened once, stepped, and read. And decision 3: the
    // body runs under a handler whose only job is to close the iterator and
    // re-raise, so a `throw` from inside the loop still reaches the
    // iterator's `return` method.
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

TEST_CASE("an arrow reaches `this` through the environment, not a parameter") {
    // Lexical `this` (docs/0012 decision 3) is capture, not an extra
    // argument: the enclosing function writes its own `this` into slot 0 of
    // its environment record, and the arrow reads it back from there. So
    // the arrow gets no `__this` parameter at all and cannot be rebound by
    // the call site.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function C() { this.v = 1; this.get = () => this.v; }\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    CHECK(printed.find("env.set %2, 0, 0, %0") != std::string::npos);
    const size_t arrow = printed.find("func __anon_fn_1(%0: dynamic)");
    REQUIRE(arrow != std::string::npos);
    CHECK(printed.find("env.get %0, 0, 0", arrow) != std::string::npos);
}

TEST_CASE("a class lowers to a constructor, a prototype and property writes") {
    // No new runtime concept: the class IS the constructor function, its
    // methods are stored on `.prototype`, and a static is stored on the
    // function itself (docs/0012 decisions 5 and 6).
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
    // installs is the one they have to land on.
    const size_t extend = printed.find("class.extend");
    const size_t protoRead = printed.find("prop.get %5, 1", extend);
    REQUIRE(extend != std::string::npos);
    CHECK(protoRead != std::string::npos);
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
    // `[a, b] = [b, a]` is a swap only if this holds. The array is built
    // first, and every `iter.at` reads THAT array — so no target write can be
    // seen by a later read (docs/0017 decision 4).
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

TEST_CASE("a default parameter is a branch, and only undefined takes it") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod =
        parseAndLower("function f(a, b = a * 2) { return b; }\nf(1);\n", diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());
    const std::string printed = il::print(*optMod);
    // A strict comparison against undefined, not a truthiness test: `null`,
    // `0` and `""` are arguments that were passed (docs/0017 decision 1).
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
    // Two IL parameters, because the rest one is still a value the body
    // reads — but the call site builds it rather than passing three
    // arguments, which is what keeps the arity fixed (docs/0017 decision 2).
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
    // denotes — an i32 escaping the operator would be a type inference has
    // no element for (docs/0015 decision 1).
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
    // The two differ at exactly one value: `if (NaN)` is false while
    // `NaN !== NaN` is true, so one op cannot answer both (docs/0015
    // decision 9).
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

TEST_CASE("a module function's locals do not leak into the module top level") {
    // The top level is a function body like any other and starts from an
    // empty scope (docs/0016 decision 3). Before this, lowering carried the
    // LAST module function's bindings into `main`, and the two faces of that
    // are both checked here. This one is the dangerous face: the read
    // resolved to a binding whose SSA value id names an unrelated
    // instruction in `main`, so it compiled and printed a plausible number.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f(p) { let secret = 42; return p + secret; }\n"
        "console.log(f(1));\n"
        "console.log(secret);\n",
        diags, buf);

    CHECK_FALSE(optMod.has_value());
    REQUIRE(diags.hasErrors());
    CHECK(diags.render(buf).find("undefined variable: secret") != std::string::npos);
}

TEST_CASE("a top-level let may share a name with a module function's local") {
    // The other face: this is ordinary JS that did not compile at all,
    // because the leaked binding made the top-level declaration look like a
    // redeclaration in the same scope.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function g() { let acc = 7; return acc; }\n"
        "let acc = 'module';\n"
        "console.log(g());\n"
        "console.log(acc);\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("a block declaration shadows an enclosing one and then uncovers it") {
    // Leaving a scope uncovers what its declarations hid; it does not delete
    // the name. `let x = 1; { let x = 2; } x` reported `undefined variable`.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function f() {\n"
        "  let x = 1;\n"
        "  { let x = 10; console.log(x); }\n"
        "  return x;\n"
        "}\n"
        "console.log(f());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    CHECK_FALSE(diags.hasErrors());
}

TEST_CASE("a top-level function declaration reaches a module-level binding") {
    // docs/0016 decision 1. The module scope is a singleton, so its record is
    // published by `main` and loaded by the module functions that need it —
    // which is what lets them stay direct-call targets.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let count = 5;\n"
        "function read() { return count; }\n"
        "function bump() { count += 1; }\n"
        "bump();\n"
        "console.log(read());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("module.env.set") != std::string::npos);
    CHECK(text.find("module.env.get") != std::string::npos);
    // Still a direct call: the whole point of not desugaring these into
    // closures (docs/0016 decision 1).
    CHECK(text.find("call @read") != std::string::npos);
}

TEST_CASE("an update expression on a captured binding goes through the environment") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function outer() { let n = 0; return () => ++n; }\n"
        "console.log(outer()());\n",
        diags, buf);

    REQUIRE(optMod.has_value());
    REQUIRE_FALSE(diags.hasErrors());
    const std::string text = il::print(*optMod);
    CHECK(text.find("env.get") != std::string::npos);
    CHECK(text.find("env.set") != std::string::npos);
}
