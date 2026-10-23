#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>

#include "lex/lexer.h"
#include "parse/parser.h"
#include "types/dump.h"
#include "types/host_contracts.h"
#include "types/infer.h"
#include "types/relational_prover.h"

using namespace bronze;

namespace {

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

TEST_CASE("relational prover: typeof and Number.isFinite branch narrowing") {
    const auto inferred = infer(
        "export function compute(x, y) {\n"
        "  if (typeof x === 'number' && Number.isFinite(y)) {\n"
        "    const sum = x + y;\n"
        "    return sum;\n"
        "  }\n"
        "  return 0;\n"
        "}\n");

    const std::string d = inferred.dump();
    CHECK(d.find("const  sum: number") != std::string::npos);
}

TEST_CASE("relational prover: early-exit guard refines fallthrough environment") {
    const auto inferred = infer(
        "export function guardedAdd(a, b) {\n"
        "  if (typeof a !== 'number' || typeof b !== 'number') {\n"
        "    return 0;\n"
        "  }\n"
        "  const total = a + b;\n"
        "  return total;\n"
        "}\n");

    const std::string d = inferred.dump();
    CHECK(d.find("const  total: number") != std::string::npos);
}

TEST_CASE("relational prover: branch reassignment guard joins to Number at merge") {
    const auto inferred = infer(
        "export function clampInput(x) {\n"
        "  if (typeof x !== 'number') {\n"
        "    x = 0;\n"
        "  }\n"
        "  const doubled = x + x;\n"
        "  return doubled;\n"
        "}\n");

    const std::string d = inferred.dump();
    CHECK(d.find("const  doubled: number") != std::string::npos);
}

TEST_CASE("relational prover: ternary typeof and Number.isInteger refinement") {
    const auto inferred = infer(
        "export function norm(x, y) {\n"
        "  const a = typeof x === 'number' ? x + 1 : 0;\n"
        "  const b = Number.isInteger(y) ? y * 2 : 1;\n"
        "  const c = a + b;\n"
        "  return c;\n"
        "}\n");

    const std::string d = inferred.dump();
    CHECK(d.find("const  a: number") != std::string::npos);
    CHECK(d.find("const  b: number") != std::string::npos);
    CHECK(d.find("const  c: number") != std::string::npos);
}

TEST_CASE("relational prover: optional numeric parameter with nullish coalescing and logical OR") {
    const auto inferred = infer(
        "function step(dt, scale, bias) {\n"
        "  const s = scale ?? 1;\n"
        "  const b = bias || 0;\n"
        "  bias ??= 0;\n"
        "  const next = dt * s + b + bias;\n"
        "  return next;\n"
        "}\n"
        "step(0.016, 2, 0.5);\n"
        "step(0.016);\n");

    const auto& r = *inferred.result;
    const auto idx = r.functionIndexOf("step");
    REQUIRE(idx.has_value());
    CHECK(r.signatureOf(*idx).returnType == types::Type::number());
    const std::string d = inferred.dump();
    CHECK(d.find("const  s: number") != std::string::npos);
    CHECK(d.find("const  b: number") != std::string::npos);
    CHECK(d.find("const  next: number") != std::string::npos);
}

TEST_CASE("host contracts: DOM, window, performance, and bro.* numeric APIs") {
    const auto inferred = infer(
        "export function measureLayout(el) {\n"
        "  const t0 = performance.now();\n"
        "  const rect = el.getBoundingClientRect();\n"
        "  const cx = rect.x + rect.width * 0.5;\n"
        "  const cy = el.getBoundingClientRect().height * 0.5 + rect.top;\n"
        "  const vp = window.innerWidth * window.devicePixelRatio + screen.width;\n"
        "  const fps = bro.compositor.fps + bro.displays.primaryScale + bro.sys.now();\n"
        "  const parsed = Number('42') + parseFloat('3.5') + parseInt('10', 10);\n"
        "  const dt = Date.now() - t0;\n"
        "  return cx + cy + vp + fps + parsed + dt;\n"
        "}\n");

    const std::string d = inferred.dump();
    CHECK(d.find("const  t0: number") != std::string::npos);
    CHECK(d.find("const  cx: number") != std::string::npos);
    CHECK(d.find("const  cy: number") != std::string::npos);
    CHECK(d.find("const  vp: number") != std::string::npos);
    CHECK(d.find("const  fps: number") != std::string::npos);
    CHECK(d.find("const  parsed: number") != std::string::npos);
    CHECK(d.find("const  dt: number") != std::string::npos);
}

TEST_CASE("host contracts: shadowed globals do not falsely claim Number") {
    const auto inferred = infer(
        "export function shadowed(performance, window) {\n"
        "  const t0 = performance.now();\n"
        "  const w = window.innerWidth;\n"
        "  return t0 + w;\n"
        "}\n");

    const std::string d = inferred.dump();
    CHECK(d.find("const  t0: dynamic") != std::string::npos);
    CHECK(d.find("const  w: dynamic") != std::string::npos);
}

TEST_CASE("host contracts: nullability classification for DOM and bro.* APIs") {
    const auto inferred = infer(
        "const a = document.getElementById('app');\n"
        "const b = document.querySelector('.item');\n"
        "const c = document.createElement('div');\n"
        "const d = c.getBoundingClientRect();\n"
        "const e = c.parentElement;\n"
        "const f = document.body;\n"
        "const g = bro.displays.byId(0);\n"
        "const h = bro.sys.info();\n");

    auto noUserBind = [](const std::string&) { return false; };
    auto getInit = [&](size_t idx) -> const ast::Expr* {
        const auto* vd = dynamic_cast<const ast::VarDecl*>(inferred.module->body[idx].get());
        REQUIRE(vd != nullptr);
        return vd->init.get();
    };

    const auto* callA = dynamic_cast<const ast::Call*>(getInit(0));
    const auto* callB = dynamic_cast<const ast::Call*>(getInit(1));
    const auto* callC = dynamic_cast<const ast::Call*>(getInit(2));
    const auto* callD = dynamic_cast<const ast::Call*>(getInit(3));
    const auto* memE = dynamic_cast<const ast::MemberAccess*>(getInit(4));
    const auto* memF = dynamic_cast<const ast::MemberAccess*>(getInit(5));
    const auto* callG = dynamic_cast<const ast::Call*>(getInit(6));
    const auto* callH = dynamic_cast<const ast::Call*>(getInit(7));

    REQUIRE((callA && callB && callC && callD && memE && memF && callG && callH));
    CHECK(types::classifyHostCallNullability(*callA, noUserBind) ==
          types::HostNullability::MaybeNullish);
    CHECK(types::classifyHostCallNullability(*callB, noUserBind) ==
          types::HostNullability::MaybeNullish);
    CHECK(types::classifyHostCallNullability(*callC, noUserBind) ==
          types::HostNullability::DefinitelyNonNullish);
    CHECK(types::classifyHostCallNullability(*callD, noUserBind) ==
          types::HostNullability::DefinitelyNonNullish);
    CHECK(types::classifyHostPropertyNullability(*memE, noUserBind) ==
          types::HostNullability::MaybeNullish);
    CHECK(types::classifyHostPropertyNullability(*memF, noUserBind) ==
          types::HostNullability::DefinitelyNonNullish);
    CHECK(types::classifyHostCallNullability(*callG, noUserBind) ==
          types::HostNullability::MaybeNullish);
    CHECK(types::classifyHostCallNullability(*callH, noUserBind) ==
          types::HostNullability::DefinitelyNonNullish);
}
