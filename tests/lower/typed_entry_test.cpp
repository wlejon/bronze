// Typed method entries (`src/lower/typed_entry.cpp`): which method-call sites
// are pointed at a copy of their target that takes unboxed arguments, and the
// verifier's check that each such edge is one the backend may emit.
//
// The edge is sound only because of two local facts — the operand IS an f64
// (or bool) in the caller's IL, and the entry is a copy of the site's own
// direct target — so the cases below pin both: sites that hold their
// arguments unboxed get the edge, sites that do not keep the generic one, and
// an edge whose operand is not what the entry takes is rejected outright.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "il/verifier.h"
#include "il/verifier_proofs.h"
#include "lower/typed_entry.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;

namespace {

il::Module lowerModule(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    return std::move(*mod);
}

const il::Function* entryOf(const il::Module& m, const std::string& origin) {
    for (const auto& fn : m.functions) {
        if (fn.typedOrigin == il::Function::kNoTypedOrigin) continue;
        if (m.functions[fn.typedOrigin].name.find(origin) != std::string::npos) return &fn;
    }
    return nullptr;
}

size_t typedEdges(const il::Module& m) {
    size_t n = 0;
    for (const auto& fn : m.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.typedTarget != il::Instruction::kNoDirectTarget) ++n;
            }
        }
    }
    return n;
}

bool verifies(const il::Module& m) {
    DiagnosticSink diags;
    return il::verifyTypedEntries(m, diags) && !diags.hasErrors();
}

// The `try` keeps `set` out of the small-function inliner (it does not inline
// a body with handlers), so the call stays a call for the edge to be on.
const char* kVec =
    "class Vec {\n"
    "  constructor() { this.x = 0; this.y = 0; this.z = 0; }\n"
    "  set(x, y, z = this.z) {\n"
    "    try { this.x = x + 1; this.y = y * 2; this.z = z; } catch (e) { throw e; }\n"
    "    return this;\n"
    "  }\n"
    "}\n";

}  // namespace

TEST_CASE("a site passing numbers reaches a typed entry of its direct target") {
    const auto m = lowerModule(std::string(kVec) +
                               "function run() {\n"
                               "  const v = new Vec();\n"
                               "  for (let i = 0; i < 10; i++) v.set(i * 0.5, i + 1, i - 1);\n"
                               "  return v.x;\n"
                               "}\n"
                               "console.log(run());\n");
    const il::Function* entry = entryOf(m, "set");
    REQUIRE(entry != nullptr);
    CHECK(entry->typedArgc == 3);
    const size_t first = entry->firstSourceParam();
    CHECK(entry->params[first].type == il::Type::F64);
    CHECK(typedEdges(m) >= 1);
    CHECK(il::print(m).find("typed @") != std::string::npos);
    CHECK(verifies(m));
}

TEST_CASE("an omitted defaulted parameter is an entry of its own arity") {
    const auto m = lowerModule(std::string(kVec) +
                               "function run() {\n"
                               "  const v = new Vec();\n"
                               "  for (let i = 0; i < 10; i++) v.set(i * 0.5, i + 1);\n"
                               "  return v.z;\n"
                               "}\n"
                               "console.log(run());\n");
    const il::Function* entry = entryOf(m, "set");
    REQUIRE(entry != nullptr);
    CHECK(entry->typedArgc == 2);
    // The parameter the edge omits reads `undefined` in the copy, so it keeps
    // the generic type: there is no value for it to be typed as.
    CHECK(entry->params[entry->firstSourceParam() + 2].type == il::Type::Dynamic);
    CHECK(verifies(m));
}

TEST_CASE("a site passing a string keeps the generic dispatch") {
    const auto m = lowerModule(std::string(kVec) +
                               "function run(s) {\n"
                               "  const v = new Vec();\n"
                               "  v.set(s, s, s);\n"
                               "  return v.x;\n"
                               "}\n"
                               "console.log(run('a'));\n");
    CHECK(typedEdges(m) == 0);
    CHECK(entryOf(m, "set") == nullptr);
}

TEST_CASE("a site passing more arguments than the method declares is not typed") {
    const auto m = lowerModule(std::string(kVec) +
                               "function run() {\n"
                               "  const v = new Vec();\n"
                               "  for (let i = 0; i < 10; i++) v.set(i * 0.5, i + 1, i, i);\n"
                               "  return v.x;\n"
                               "}\n"
                               "console.log(run());\n");
    CHECK(typedEdges(m) == 0);
}

TEST_CASE("the verifier rejects a typed edge whose operand is not the entry's type") {
    auto m = lowerModule(std::string(kVec) +
                         "function run() {\n"
                         "  const v = new Vec();\n"
                         "  for (let i = 0; i < 10; i++) v.set(i * 0.5, i + 1, i - 1);\n"
                         "  return v.x;\n"
                         "}\n"
                         "console.log(run());\n");
    REQUIRE(verifies(m));
    // Point the first typed operand at the receiver, a Dynamic value: the one
    // way the backend would hand a boxed word to an f64 parameter.
    bool forged = false;
    for (auto& fn : m.functions) {
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (forged || inst.typedTarget == il::Instruction::kNoDirectTarget) continue;
                inst.operands[1] = inst.operands[0];
                forged = true;
            }
        }
    }
    REQUIRE(forged);
    CHECK_FALSE(verifies(m));
}

TEST_CASE("the verifier rejects a typed edge to an entry of another method") {
    auto m = lowerModule(std::string(kVec) +
                         "function run() {\n"
                         "  const v = new Vec();\n"
                         "  for (let i = 0; i < 10; i++) v.set(i * 0.5, i + 1, i - 1);\n"
                         "  return v.x;\n"
                         "}\n"
                         "console.log(run());\n");
    bool forged = false;
    for (auto& fn : m.functions) {
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (forged || inst.typedTarget == il::Instruction::kNoDirectTarget) continue;
                // The constructor: a function that is no typed entry at all.
                inst.directTarget = inst.directTarget == 0 ? 1 : 0;
                forged = true;
            }
        }
    }
    REQUIRE(forged);
    CHECK_FALSE(verifies(m));
}
