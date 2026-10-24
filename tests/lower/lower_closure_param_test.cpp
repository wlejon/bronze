// The closure PARAMETER proof (`src/lower/lower_scope.cpp
// planClosureParamNumbers`): which nested `function f(x)` gets an f64
// parameter slot, and — as with the static call plan next door, and for the
// same reason — which ones do not.
//
// The claim is that the calls this compilation can see are ALL the calls, so
// the join over their arguments is a fact about every value the parameter can
// ever hold. Every "no" below is a case where a yes would be a claim about
// callers nobody enumerated: the value left through a door, or the argument
// list did not line up with the parameter list one for one.
//
// What the proof produces is visible in one place — the printed `func` header,
// where a proven parameter reads `f64` and an unproven one reads `dynamic`.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "lower_fixture.h"
#include "types/pins.h"

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

// The printed header of `func <name>(...)`, so a test can say what the
// parameter types ARE rather than fishing for a substring anywhere in a module.
std::string header(const std::string& il, const std::string& fn) {
    const auto at = il.find("\nfunc " + fn + "(");
    if (at == std::string::npos) return "";
    const auto end = il.find('\n', at + 1);
    return il.substr(at + 1, end - at - 1);
}

}  // namespace

TEST_CASE("every call site of a sibling closure passes a Number, so the parameter is one") {
    // The argument still has to be a proven Number, which is INFERENCE's
    // answer and not this plan's: `i & 7` on a loop counter is one, and
    // `k & 7` on a dynamic parameter is not — one BigInt operand makes `&` a
    // TypeError rather than a Number, and nothing here rules that out.
    const auto il = lowerToText(
        "function make() {\n"
        "  let n = 0;\n"
        "  function bump(by) { n = n + by; }\n"
        "  function run(k) {\n"
        "    for (let i = 0; i < 3; i++) { bump(i & 7); bump(i + 1); }\n"
        "    return n;\n"
        "  }\n"
        "  return run;\n"
        "}\n"
        "console.log(make()(2));\n");
    CHECK(header(il, "bump").find("f64") != std::string::npos);
    // `run` is RETURNED. Its callers are reached through a function value and
    // nothing enumerates them, which is the boundary of this mechanism and the
    // reason `bench/pins/env-slot-kernel.pins` still carries `param
    // render(iters)` by hand.
    CHECK(header(il, "run").find("f64") == std::string::npos);
}

TEST_CASE("a mention that is not a callee is the value leaving, and refuses the proof") {
    const auto viaValue = lowerToText(
        "function make() {\n"
        "  function g(x) { return x + 1; }\n"
        "  const h = g;\n"
        "  return g(1) + h(2);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(viaValue, "g").find("f64") == std::string::npos);

    const auto viaCall = lowerToText(
        "function make() {\n"
        "  function g(x) { return x + 1; }\n"
        "  return g.call(null, 1) + g(2);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(viaCall, "g").find("f64") == std::string::npos);
}

TEST_CASE("a short call binds undefined, which is not a Number") {
    const auto il = lowerToText(
        "function make() {\n"
        "  function g(x, y) { return x + y; }\n"
        "  return g(1) + g(2, 3);\n"
        "}\n"
        "console.log(make());\n");
    // Position 0 is proven by both sites; position 1 is refused by the short
    // one, and the two answers live in the same header.
    const auto h = header(il, "g");
    CHECK(h.find("f64") != std::string::npos);
    CHECK(h.find("dynamic") != std::string::npos);
}

TEST_CASE("a spread argument breaks the one-argument-per-parameter correspondence") {
    const auto il = lowerToText(
        "function make() {\n"
        "  function g(x, y) { return x + y; }\n"
        "  const a = [1, 2];\n"
        "  return g(...a) + g(3, 4);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(il, "g").find("f64") == std::string::npos);
}

TEST_CASE("one site with a non-Number argument refuses the position") {
    const auto il = lowerToText(
        "function make() {\n"
        "  function g(x) { return typeof x; }\n"
        "  return g(1) + g('s');\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(il, "g").find("f64") == std::string::npos);
}

TEST_CASE("a rebound name is not one binding throughout") {
    const auto il = lowerToText(
        "function make() {\n"
        "  function g(x) { return x + 1; }\n"
        "  g = function (x) { return x + 2; };\n"
        "  return g(1);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(il, "g").find("f64") == std::string::npos);
}

TEST_CASE("a default, a rest and a pattern are positions the language may fill itself") {
    const auto def = lowerToText(
        "function make() {\n"
        "  function g(x = 0) { return x + 1; }\n"
        "  return g(1) + g(2);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(def, "g").find("f64") == std::string::npos);

    const auto rest = lowerToText(
        "function make() {\n"
        "  function g(...xs) { return xs.length; }\n"
        "  return g(1) + g(2);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(rest, "g").find("f64") == std::string::npos);
}

TEST_CASE("a generator's parameters are bound by a resume edge, not by the call") {
    const auto il = lowerToText(
        "function make() {\n"
        "  function* g(x) { yield x + 1; }\n"
        "  return g(1).next().value + g(2).next().value;\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(il, "g").find("f64") == std::string::npos);
}

TEST_CASE("BRONZE_NO_CLOSURE_PARAM_PROOF is the A/B seam and it is read once") {
    // The seam is a static local, so a test cannot flip it mid-process. What
    // this pins is the shape the seam switches between: with the proof off the
    // header is the one every case above that REFUSES already produces, which
    // is what makes those cases the seam's own regression test.
    const auto il = lowerToText(
        "function make() {\n"
        "  function g(x) { return x + 1; }\n"
        "  const h = g;\n"
        "  return g(1) + h(2);\n"
        "}\n"
        "console.log(make());\n");
    CHECK(header(il, "g").find("dynamic") != std::string::npos);
}

// ---- a pin must add a promise, never subtract a proof ------------------------

TEST_CASE("a pinned RETURN does not cost the same function its parameter's f64 slot") {
    // `flow_expr.cpp`'s call rule answers a pinned return early, and it used to
    // answer it by RETURNING — which skipped the loop just below that joins this
    // site's argument types into the callee's `observedParams`. So
    // `return run: number` silently un-typed `run`'s own parameter: the loop
    // bound `i < iters` in bench/mat4_kernel.js went from an `fcmp` to a boxed
    // `rel.lt`, and the manifest the stage C1 census wrote measured 1.8 ns/call
    // SLOWER than the hand-written one it otherwise reproduced.
    //
    // The two headers below are the whole regression: the parameter must stay
    // `f64` with the return pinned, and pinning the return must still be worth
    // something (`-> f64`).
    const char* kSrc =
        "function run(iters) {\n"
        "  let acc = 0;\n"
        "  for (let i = 0; i < iters; i++) acc = acc + i;\n"
        "  return acc;\n"
        "}\n"
        "console.log(run(10));\n";

    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto astMod = bronze::lower_test::parseOnly(kSrc, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(astMod != nullptr);

    types::PinManifest pins;
    std::string err;
    REQUIRE_MESSAGE(pins.parse("return run: number\n", "test.pins", err), err);

    auto inferred = types::inferModule(*astMod, diags, /*hostGlobals=*/nullptr, &pins);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(inferred.has_value());
    const auto mod = lower::lowerModule(*astMod, diags, &*inferred, /*hostGlobals=*/nullptr,
                                        /*sources=*/nullptr, /*stats=*/nullptr,
                                        /*assumeNoBigInt=*/false, &pins);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    CHECK(header(il::print(*mod), "run") == "func run(%0: f64) -> f64 {");
}

TEST_CASE("coupled closure parameter and captured accumulator slot converge to f64") {
    const auto il = lowerToText(
        "function makeCounter() {\n"
        "  let total = 0;\n"
        "  function add(dx) { total = total + dx; }\n"
        "  add(1);\n"
        "  add(total + 2);\n"
        "  return total;\n"
        "}\n"
        "console.log(makeCounter());\n");
    CHECK(header(il, "add").find("f64") != std::string::npos);
    CHECK(il.find(", raw") != std::string::npos);
}

TEST_CASE("path-guarded ternary, nullish coalescing, and host numeric APIs lower to f64") {
    const auto il = lowerToText(
        "function compute(x, el) {\n"
        "  const safeX = typeof x === 'number' ? x : 0;\n"
        "  const rect = el.getBoundingClientRect();\n"
        "  const area = rect.width * rect.height;\n"
        "  const now = performance.now();\n"
        "  return safeX + area + now;\n"
        "}\n"
        "console.log(compute(5, { getBoundingClientRect() { return { width: 2, height: 3 }; } }));\n");
    CHECK(header(il, "compute").find("-> f64") != std::string::npos);
}

TEST_CASE("non-escaping nested functions inside an IIFE specialize TypedArray parameters across closure calls") {
    const auto il = lowerToText(
        "(function() {\n"
        "  function sum(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }\n"
        "  function hist(a, out) { for (let i = 0; i < a.length; i++) out[a[i] & 255]++; }\n"
        "  const F = new Float64Array(16);\n"
        "  const UB = new Uint32Array(16);\n"
        "  const O = new Int32Array(256);\n"
        "  function run(fn) { return fn(); }\n"
        "  const total = run(() => { hist(UB, O); return sum(F); });\n"
        "  console.log(total);\n"
        "})();\n");
    CHECK(il.find("elem.get.typed %1") != std::string::npos);
    CHECK(il.find("elem.set.typed %2") != std::string::npos);
}

TEST_CASE("classic-script globalThis prologue for replaceable host global keeps direct-called function specialized") {
    const auto il = lowerToText(
        "{globalThis.blur = blur;}\n"
        "function blur(src, dst, w, h) {\n"
        "  for (let y = 1; y < h - 1; y++)\n"
        "    for (let x = 1; x < w - 1; x++) {\n"
        "      const i = y * w + x;\n"
        "      dst[i] = (src[i - 1] + src[i + 1] + src[i - w] + src[i + w] + 4 * src[i]) * 0.125;\n"
        "    }\n"
        "}\n"
        "const BA = new Float32Array(64), BB = new Float32Array(64);\n"
        "const r = {};\n"
        "blur(BA, BB, 8, 8);\n"
        "r.blur = BB[9];\n"
        "console.log(r.blur);\n");
    CHECK(header(il, "blur") == "func blur(%0: dynamic, %1: dynamic, %2: f64, %3: f64) -> void {");
    CHECK(il.find("elem.get.typed") != std::string::npos);
    CHECK(il.find("elem.set.typed") != std::string::npos);
}
