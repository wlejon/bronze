// REPEATED READS (src/lower/read_cse.h): a read an earlier one of the same
// receiver and key dominates over nothing that writes or runs code is that
// earlier read.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "il/verifier.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::parseAndLower;

namespace {

// How many reads of `key` the function named `name` keeps.
size_t readsOf(const il::Module& mod, const std::string& name, const std::string& key) {
    const size_t k = lower_test::keyIndex(mod, key);
    size_t n = 0;
    for (const auto& fn : mod.functions) {
        if (fn.name != name) continue;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::PropGet && inst.keyIndex == k) ++n;
            }
        }
    }
    return n;
}

// How many binding reads the function named `name` keeps.
size_t bindingReads(const il::Module& mod, const std::string& name) {
    size_t n = 0;
    for (const auto& fn : mod.functions) {
        if (fn.name != name) continue;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::EnvGet || inst.op == il::Op::EnvGetTdz) ++n;
            }
        }
    }
    return n;
}

il::Module lowered(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.js", "");
    auto mod = parseAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    DiagnosticSink verify;
    CHECK(il::verify(*mod, verify));
    return std::move(*mod);
}

}  // namespace

TEST_CASE("a comparator's second read of each side is its first") {
    const il::Module mod = lowered(
        "function cmp(a, b) {\n"
        "  if (a.order !== b.order) return a.order - b.order;\n"
        "  return a.id - b.id;\n"
        "}\n");
    CHECK(readsOf(mod, "cmp", "order") == 2);
    CHECK(readsOf(mod, "cmp", "id") == 2);
}

TEST_CASE("a call between two reads keeps both") {
    const il::Module mod = lowered(
        "function f(a, g) {\n"
        "  const x = a.order;\n"
        "  g();\n"
        "  return x + a.order;\n"
        "}\n");
    CHECK(readsOf(mod, "f", "order") == 2);
}

TEST_CASE("a store between two reads keeps both") {
    const il::Module mod = lowered(
        "function f(a, b) {\n"
        "  const x = a.order;\n"
        "  b.order = 1;\n"
        "  return x === a.order;\n"
        "}\n");
    CHECK(readsOf(mod, "f", "order") == 2);
}

TEST_CASE("a key some getter is installed under is read every time") {
    const il::Module mod = lowered(
        "class C { get order() { return Math.random(); } }\n"
        "function f(a) {\n"
        "  return a.order === a.order;\n"
        "}\n"
        "new C();\n");
    CHECK(readsOf(mod, "f", "order") == 2);
}

TEST_CASE("a function that builds a Proxy folds no read; the rest of the module does") {
    const il::Module mod = lowered(
        "function f(a) {\n"
        "  return a.order === a.order;\n"
        "}\n"
        "function g(a) {\n"
        "  const p = new Proxy({}, {});\n"
        "  return a.order === a.order ? p : null;\n"
        "}\n");
    CHECK(readsOf(mod, "f", "order") == 1);
    CHECK(readsOf(mod, "g", "order") == 2);
}

TEST_CASE("a read on both arms of a branch is the read before it") {
    const il::Module mod = lowered(
        "function f(a, c) {\n"
        "  const x = a.order;\n"
        "  let y = 0;\n"
        "  if (c === 1) { y = 1; } else { y = 2; }\n"
        "  return a.order === x ? y : 3;\n"
        "}\n");
    CHECK(readsOf(mod, "f", "order") == 1);
}

TEST_CASE("a closure's binding read twice over nothing is read once") {
    const il::Module mod = lowered(
        "function outer() {\n"
        "  let target = null;\n"
        "  function inner(a) {\n"
        "    if (target === null) return a;\n"
        "    return target === a ? target : a;\n"
        "  }\n"
        "  return inner;\n"
        "}\n"
        "outer()(1);\n");
    CHECK(bindingReads(mod, "inner") == 1);
}

TEST_CASE("a binding written between two reads is read again") {
    const il::Module mod = lowered(
        "function outer() {\n"
        "  let target = null;\n"
        "  function inner(a) {\n"
        "    const x = target;\n"
        "    target = a;\n"
        "    return x === target;\n"
        "  }\n"
        "  return inner;\n"
        "}\n"
        "outer()(1);\n");
    CHECK(bindingReads(mod, "inner") == 2);
}

TEST_CASE("a key defined with a data descriptor still folds") {
    const il::Module mod = lowered(
        "class M { constructor(i) { Object.defineProperty(this, 'id', { value: i }); this.order = 0; } }\n"
        "function f(a) {\n"
        "  return a.id === a.id && a.order === a.order;\n"
        "}\n"
        "new M(1);\n");
    CHECK(readsOf(mod, "f", "id") == 1);
    CHECK(readsOf(mod, "f", "order") == 1);
}

TEST_CASE("a key defined with a getter descriptor is read every time") {
    const il::Module mod = lowered(
        "function install(o) { Object.defineProperty(o, 'order', { get() { return Math.random(); } }); }\n"
        "function f(a) {\n"
        "  return a.order === a.order;\n"
        "}\n"
        "install({});\n");
    CHECK(readsOf(mod, "f", "order") == 2);
}

TEST_CASE("a define under a key not spelled out folds nothing") {
    const il::Module mod = lowered(
        "function install(o, k) { Object.defineProperty(o, k, { get() { return Math.random(); } }); }\n"
        "function f(a) {\n"
        "  return a.order === a.order;\n"
        "}\n"
        "install({}, 'order');\n");
    CHECK(readsOf(mod, "f", "order") == 2);
}

TEST_CASE("a read through a folded read folds too") {
    const il::Module mod = lowered(
        "function cmp(a, b) {\n"
        "  if (a.material.id !== b.material.id) return a.material.id - b.material.id;\n"
        "  return 0;\n"
        "}\n");
    CHECK(readsOf(mod, "cmp", "material") == 2);
    CHECK(readsOf(mod, "cmp", "id") == 2);
}

TEST_CASE("a getter map with a computed field folds nothing") {
    const il::Module mod = lowered(
        "function install(o, k) { Object.defineProperties(o, { [k]: { get() { return Math.random(); } } }); }\n"
        "function f(a) {\n"
        "  return a.order === a.order;\n"
        "}\n"
        "install({}, 'order');\n");
    CHECK(readsOf(mod, "f", "order") == 2);
}
