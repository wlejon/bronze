#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

namespace {

const il::Function* findFunction(const il::Module& mod, const std::string& name) {
    for (const auto& fn : mod.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

bool blockHasPropGet(const il::Block& blk) {
    for (const auto& inst : blk.instructions) {
        if (inst.op == il::Op::PropGet) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("loop property hoisting does not speculatively hoist guarded nullable property read") {
    // Issue 1: `while (o !== null) { o.prop; }` with nullable receiver `o`.
    // The block containing `o.prop` is in the body, which is NOT guaranteed to execute.
    // Speculatively hoisting it into the preheader would cause eager TypeError when o is null.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(o) {\n"
        "  while (o !== null) {\n"
        "    let x = o.prop;\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Block 0 is entry/preheader jumping to header b1.
    // b0 must NOT contain PropGet!
    CHECK_FALSE(blockHasPropGet(fn->blocks[0]));

    // PropGet must remain in a subsequent body block
    bool foundInBody = false;
    for (size_t b = 1; b < fn->blocks.size(); ++b) {
        if (blockHasPropGet(fn->blocks[b])) {
            foundInBody = true;
            break;
        }
    }
    CHECK(foundInBody);
}

TEST_CASE("loop property hoisting does not hoist property read guarded by conditional if") {
    // `for (...) { if (o !== null) o.prop; }`
    // `o.prop` is inside the conditional block, not guaranteed to execute.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(o) {\n"
        "  for (let i = 0; i < 10; i++) {\n"
        "    if (o !== null) {\n"
        "      let x = o.prop;\n"
        "    }\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Preheader is block 0 (entry before the loop). It must NOT contain PropGet.
    CHECK_FALSE(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting hoists invariant property read on provably non-nullish object") {
    // `o` is a known newly allocated object, provably non-nullish.
    // `o.prop` can be safely hoisted into preheader.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(cond) {\n"
        "  let m = Math;\n"
        "  let sum = 0;\n"
        "  while (cond) {\n"
        "    sum += m.PI;\n"
        "  }\n"
        "  return sum;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    INFO(il::print(*optMod));
    CHECK(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting hoists property read in loop header before conditional branch") {
    // `while (o.prop > 0) { ... }`
    // `o.prop` is in the loop header block, which is guaranteed to execute whenever the loop is entered.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(o) {\n"
        "  while (o.prop > 0) {\n"
        "    let y = 1;\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Hoisted to preheader (b0)
    CHECK(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting detects interprocedural aliasing mutations") {
    // Issue 2: A callee mutates property "x". The receiver `obj` is passed into `test`,
    // so it is NOT a proven non-escaping local object. The callee could mutate `obj.x`
    // via global or heap aliasing. Property hoisting must NOT hoist `obj.x` across the call.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let g = { x: 1 };\n"
        "function mutateX() {\n"
        "  g.x = 99;\n"
        "}\n"
        "function test(obj) {\n"
        "  for (let i = 0; i < 10; i++) {\n"
        "    mutateX();\n"
        "    let val = obj.x;\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Preheader of loop in `test` must NOT contain PropGet for x!
    CHECK_FALSE(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting permits hoisting for proven non-escaping local object across call mutating same key") {
    // When `local` is a newly created local object that does NOT escape,
    // a call that mutates "x" on other objects cannot mutate `local.x`.
    // Hoisting is permitted.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let g = { x: 1 };\n"
        "function mutateX() {\n"
        "  g.x = 99;\n"
        "}\n"
        "function getX() { return 10; }\n"
        "function test() {\n"
        "  let local = { x: getX() };\n"
        "  let sum = 0;\n"
        "  for (let i = 0; i < 10; i++) {\n"
        "    mutateX();\n"
        "    sum += local.x;\n"
        "  }\n"
        "  return sum;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Inside the loop, local.x was hoisted out, so no PropGet remains in the loop blocks!
    bool loopBodyHasPropGet = false;
    for (size_t b = 1; b < fn->blocks.size(); ++b) {
        if (blockHasPropGet(fn->blocks[b])) loopBodyHasPropGet = true;
    }
    CHECK_FALSE(loopBodyHasPropGet);
}

TEST_CASE("module-wide reflection does not disable hoisting in independent functions") {
    // Issue 3: Mentioning Proxy or reflection keys in one function must not disable
    // loop property hoisting in independent functions that do not touch reflection.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function usesReflection() {\n"
        "  let p = new Proxy({}, {});\n"
        "  return p;\n"
        "}\n"
        "function independent(cond) {\n"
        "  let m = Math;\n"
        "  let sum = 0;\n"
        "  while (cond) {\n"
        "    sum += m.PI;\n"
        "  }\n"
        "  return sum;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "independent");
    REQUIRE(fn != nullptr);

    // Hoisting should have succeeded in independent function's preheader (b0)
    CHECK(blockHasPropGet(fn->blocks[0]));
}
