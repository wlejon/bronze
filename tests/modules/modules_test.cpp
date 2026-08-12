// The module graph: resolution, the graph's shape and order, the export tables,
// and — the one that matters most — that an import binding is renamed to the
// exporting file's binding rather than copied.
//
// These drive `loadProgram` and read the merged AST dump, which is the
// artefact that shows the renaming. What the merged module MEANS at run time
// is the oracle suite's job (tests/oracle/cases/module_*).

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ast/dump.h"
#include "modules/modules.h"

using namespace bronze;

namespace {

// One temp directory per test, removed on the way out, so two cases can never
// see each other's files.
class Sandbox {
public:
    explicit Sandbox(const std::string& name)
        : root_(std::filesystem::temp_directory_path() / ("bronze_modules_" + name)) {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::string write(const std::string& relative, const std::string& text) const {
        std::filesystem::path path = root_ / relative;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path.string();
    }

private:
    std::filesystem::path root_;
};

struct Loaded {
    std::string dump;
    std::string errors;
    bool ok = false;
};

Loaded load(const std::string& entry) {
    Loaded result;
    SourceSet sources;
    DiagnosticSink diags;
    auto module = modules::loadProgram(entry, sources, diags);
    result.errors = diags.render(sources);
    if (module) {
        result.ok = true;
        result.dump = ast::dump(*module);
    }
    return result;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a single file with no imports is passed through unrenamed") {
    Sandbox box("single");
    const std::string entry = box.write("main.js", "let x = 1;\nconsole.log(x);\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "(let x"));
    CHECK_FALSE(contains(r.dump, "mod1."));
}

TEST_CASE("an imported binding is renamed to the exporting file's binding") {
    Sandbox box("live");
    box.write("counter.js", "export let n = 0;\nexport function bump() { n += 1; }\n");
    const std::string entry =
        box.write("main.js", "import { n, bump } from './counter.js';\nbump();\nconsole.log(n);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    // One binding, named once. The importer's `n` and the exporter's `n` are
    // the same string, which is what makes the view live.
    CHECK(contains(r.dump, "(let mod1.n"));
    CHECK(contains(r.dump, "(ident mod1.n)"));
    CHECK(contains(r.dump, "(function mod1.bump"));
}

TEST_CASE("dependencies are evaluated before the importer") {
    Sandbox box("order");
    box.write("a.js", "console.log('a');\nexport const A = 1;\n");
    const std::string entry = box.write("main.js", "import { A } from './a.js';\nconsole.log(A);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(r.dump.find("(string \"a\")") < r.dump.find("mod1.A"));
}

TEST_CASE("a name shadowed inside a function is not renamed") {
    Sandbox box("shadow");
    box.write("lib.js",
              "export let v = 1;\n"
              "export function pick() { let v = 99; return v; }\n");
    const std::string entry = box.write("main.js", "import { pick } from './lib.js';\npick();\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "(let mod1.v"));
    CHECK(contains(r.dump, "(let v"));      // the function's own binding
    CHECK(contains(r.dump, "(ident v)"));   // and the reference to it
}

TEST_CASE("a file imported twice is one module") {
    Sandbox box("diamond");
    box.write("shared.js", "export const S = 1;\n");
    box.write("left.js", "import { S } from './shared.js';\nexport const L = S;\n");
    box.write("right.js", "import { S } from './shared.js';\nexport const R = S;\n");
    const std::string entry = box.write(
        "main.js", "import { L } from './left.js';\nimport { R } from './right.js';\n"
                   "console.log(L + R);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    size_t first = r.dump.find("const mod2.S");
    REQUIRE(first != std::string::npos);
    CHECK(r.dump.find("const mod2.S", first + 1) == std::string::npos);
}

TEST_CASE("a bare specifier is a named error") {
    Sandbox box("bare");
    const std::string entry = box.write("main.js", "import x from 'three';\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "unsupported module specifier \"three\""));
    CHECK(contains(r.errors, "relative specifiers only"));
}

TEST_CASE("a missing file is a named error naming the path") {
    Sandbox box("missing");
    const std::string entry = box.write("main.js", "import x from './nope.js';\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "cannot resolve module specifier \"./nope.js\""));
    CHECK(contains(r.errors, "nope.js"));
}

TEST_CASE("a cycle links, and its back edge names the same binding as its forward edge") {
    // The back edge b.js -> a.js closes the loop and resolves to a.js's own
    // binding, exactly as main.js's forward edge does. One slot, reached by two
    // paths, which is what "an import is a live view" means when the graph is
    // not a tree. What makes it SAFE rather than merely possible is the
    // temporal dead zone: `a` holds the uninitialized marker until a.js's own
    // declaration runs (tests/oracle/cases/module_cycle*).
    Sandbox box("cycle");
    box.write("a.js", "import { b } from './b.js';\nexport const a = 1;\n");
    box.write("b.js", "import { a } from './a.js';\nexport const b = 2;\nexport const seen = a;\n");
    const std::string entry = box.write("main.js", "import { a } from './a.js';\nconsole.log(a);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "const mod1.a"));
    CHECK(contains(r.dump, "const mod2.b"));
    // b.js's reference to the imported `a` is a.js's binding, not a copy.
    CHECK(contains(r.dump, "mod1.a"));
}

TEST_CASE("a cycle evaluates its deepest member first") {
    // The post-order of the walk is the evaluation order, cycle or not: b.js is
    // the end the walk leaves first, so its statements are merged ahead of
    // a.js's. That order is what decides which crossing read lands in a dead
    // zone, so it is pinned here rather than left to the merge.
    Sandbox box("cycleorder");
    box.write("a.js", "import { b } from './b.js';\nexport const a = 1;\n");
    box.write("b.js", "import { a } from './a.js';\nexport const b = 2;\n");
    const std::string entry = box.write("main.js", "import { a } from './a.js';\nconsole.log(a);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    const size_t bAt = r.dump.find("const mod2.b");
    const size_t aAt = r.dump.find("const mod1.a");
    REQUIRE(bAt != std::string::npos);
    REQUIRE(aAt != std::string::npos);
    CHECK(bAt < aAt);
}

TEST_CASE("a self-import is a cycle of one and links") {
    // The tightest cycle there is, and the one that would hang a loader that
    // followed the back edge. The alias is not decoration: `import { a }` into
    // a file that also declares `a` is a duplicate declaration whatever the
    // graph looks like.
    Sandbox box("selfcycle");
    const std::string entry = box.write(
        "main.js",
        "import { a as self } from './main.js';\nexport const a = 1;\nconsole.log(self);\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "const a"));
}

TEST_CASE("a missing export is a named error") {
    Sandbox box("noexport");
    box.write("lib.js", "export const a = 1;\n");
    const std::string entry = box.write("main.js", "import { b } from './lib.js';\nconsole.log(b);\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "has no export named 'b'"));
}

TEST_CASE("a duplicate export is a named error") {
    Sandbox box("dupexport");
    box.write("lib.js", "export const a = 1;\nconst z = 2;\nexport { z as a };\n");
    const std::string entry = box.write("main.js", "import { a } from './lib.js';\nconsole.log(a);\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "duplicate export 'a'"));
}

TEST_CASE("an import and a declaration of the same name is a named error") {
    Sandbox box("collide");
    box.write("lib.js", "export const a = 1;\n");
    const std::string entry = box.write("main.js", "import { a } from './lib.js';\nlet a = 2;\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "is imported and also declared"));
}

TEST_CASE("a diagnostic in a dependency names that file") {
    Sandbox box("diagfile");
    box.write("lib.js", "export const a = ;\n");
    const std::string entry = box.write("main.js", "import { a } from './lib.js';\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "lib.js:1:"));
}

TEST_CASE("default and namespace imports resolve") {
    Sandbox box("defaultns");
    box.write("lib.js", "export default 42;\nexport const k = 1;\n");
    const std::string entry = box.write(
        "main.js", "import d, * as ns from './lib.js';\nconsole.log(d, ns.k);\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "mod1.default"));
    // The namespace is an object literal of getters over the target's bindings,
    // in the target's export order.
    CHECK(contains(r.dump, "(const ns"));
    CHECK(contains(r.dump, "(ident mod1.k)"));
}

TEST_CASE("a write through a namespace binding is refused by name") {
    Sandbox box("nswrite");
    box.write("lib.js", "export let k = 1;\n");
    const std::string entry =
        box.write("main.js", "import * as ns from './lib.js';\nns.k = 2;\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "read-only"));
}

TEST_CASE("export * from re-exports every name but default") {
    Sandbox box("starfrom");
    box.write("lib.js", "export const a = 1;\nexport default 2;\n");
    box.write("barrel.js", "export * from './lib.js';\n");
    const std::string entry =
        box.write("main.js", "import { a } from './barrel.js';\nconsole.log(a);\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "mod2.a"));

    const std::string bad = box.write("bad.js", "import d from './barrel.js';\nconsole.log(d);\n");
    Loaded r2 = load(bad);
    CHECK_FALSE(r2.ok);
    CHECK(contains(r2.errors, "has no export named 'default'"));
}

TEST_CASE("an import outside the module top level is a syntax error") {
    Sandbox box("nested");
    const std::string entry = box.write("main.js", "{ import { a } from './x.js'; }\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "top level of a module"));
}

// The renamer walks `new`'s callee as an EXPRESSION now that the grammar admits
// one. Getting this wrong is the worst failure this walk has: an unrenamed base
// binds to whatever the importing file happens to call `registry`, which is a
// silent wrong binding and not a diagnostic. Both halves are pinned — the base
// is renamed, and the property name beside it, which is a key and never a
// binding, is not.
TEST_CASE("a new callee that is a member of an imported binding is renamed") {
    Sandbox box("newcallee");
    box.write("registry.js", "export function Ctor() { this.k = 1; }\nexport const table = { Ctor };\n");
    const std::string entry =
        box.write("main.js", "import { table } from './registry.js';\n"
                             "const made = new table.Ctor();\n"
                             "console.log(made.k);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "(const mod1.table"));
    CHECK(contains(r.dump, "(ident mod1.table)"));
    CHECK(contains(r.dump, "(member .Ctor"));
    CHECK_FALSE(contains(r.dump, "(ident table)"));
}

// The same walk, one level deeper and through a computed key: the INDEX is an
// ordinary expression and an imported binding used as one must be renamed too.
TEST_CASE("both halves of a computed new callee are renamed") {
    Sandbox box("newindex");
    box.write("registry.js",
              "export function Ctor() { this.k = 2; }\n"
              "export const table = { Ctor };\nexport const which = 'Ctor';\n");
    const std::string entry =
        box.write("main.js", "import { table, which } from './registry.js';\n"
                             "console.log(new table[which]().k);\n");

    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "(ident mod1.table)"));
    CHECK(contains(r.dump, "(ident mod1.which)"));
    CHECK_FALSE(contains(r.dump, "(ident table)"));
    CHECK_FALSE(contains(r.dump, "(ident which)"));
}
