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

// ---- bare specifiers -------------------------------------------------------
//
// `cases/module_bare_specifier` pins that a package resolves and runs. What it
// cannot show is the WALK — one package in one node_modules is the same answer
// under any rule — nor any of the refusals, since a case that refuses does not
// print. Both are here.

TEST_CASE("a bare specifier resolves through the package's \"main\"") {
    Sandbox box("bare_main");
    box.write("node_modules/lib/package.json", "{ \"name\": \"lib\", \"main\": \"index.js\" }\n");
    box.write("node_modules/lib/index.js", "export const k = 1;\n");
    const std::string entry =
        box.write("main.js", "import { k } from 'lib';\nconsole.log(k);\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "mod1.k"));
}

TEST_CASE("a bare specifier resolves through a string \"exports\"") {
    Sandbox box("bare_exports");
    box.write("node_modules/lib/package.json", "{ \"exports\": \"./entry.js\" }\n");
    box.write("node_modules/lib/entry.js", "export const k = 1;\n");
    const std::string entry = box.write("main.js", "import { k } from 'lib';\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "mod1.k"));
}

TEST_CASE("a bare specifier resolves through an \"exports\" subpath map") {
    Sandbox box("bare_subpath");
    box.write("node_modules/lib/package.json",
              "{ \"exports\": { \".\": \"./root.js\", \"./extra\": \"./extra.js\" } }\n");
    box.write("node_modules/lib/root.js", "export const root = 1;\n");
    box.write("node_modules/lib/extra.js", "export const extra = 2;\n");
    const std::string entry =
        box.write("main.js", "import { root } from 'lib';\nimport { extra } from 'lib/extra';\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, ".root"));
    CHECK(contains(r.dump, ".extra"));
}

TEST_CASE("the NEAREST node_modules wins") {
    // The one step of the algorithm with a single answer, and the only one
    // worth implementing rather than refusing. Two packages of the same name,
    // and which one an importer gets is decided by where the importer IS —
    // getting this backwards is a program that builds and is not the one on
    // disk, which is what every refusal in this file exists to avoid.
    Sandbox box("bare_nearest");
    box.write("node_modules/lib/package.json", "{ \"main\": \"index.js\" }\n");
    box.write("node_modules/lib/index.js", "export const who = 'outer';\n");
    box.write("sub/node_modules/lib/package.json", "{ \"main\": \"index.js\" }\n");
    box.write("sub/node_modules/lib/index.js", "export const who = 'inner';\n");
    box.write("sub/inner.js", "import { who } from 'lib';\nexport const seen = who;\n");
    const std::string entry =
        box.write("main.js", "import { who } from 'lib';\nimport { seen } from './sub/inner.js';\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    // Three files past the entry: the outer package, sub/inner.js and the
    // inner package. If the walk stopped at the first `node_modules` it found
    // for both importers, there would be one package and one fewer module.
    CHECK(contains(r.dump, "mod3."));
}

TEST_CASE("a package that is nowhere on the way up is a named error listing where it looked") {
    Sandbox box("bare_missing");
    const std::string entry = box.write("main.js", "import x from 'three';\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "no package named \"three\""));
    CHECK(contains(r.errors, "node_modules"));
}

TEST_CASE("a conditional exports object is refused by name") {
    // The refusal this whole feature is shaped around. Reading the map means
    // choosing a condition set, and a condition chosen differently from the way
    // the package was written resolves to a different entry point WITHOUT any
    // error — a different program, silently.
    Sandbox box("bare_conditional");
    box.write("node_modules/lib/package.json",
              "{ \"exports\": { \"import\": \"./esm.js\", \"require\": \"./cjs.js\" } }\n");
    box.write("node_modules/lib/esm.js", "export const k = 1;\n");
    box.write("node_modules/lib/cjs.js", "export const k = 2;\n");
    const std::string entry = box.write("main.js", "import { k } from 'lib';\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "CONDITIONAL exports object"));

    // The same refusal one level down, where a subpath maps to conditions.
    Sandbox nested("bare_conditional_sub");
    nested.write("node_modules/lib/package.json",
                 "{ \"exports\": { \".\": { \"import\": \"./esm.js\" } } }\n");
    nested.write("node_modules/lib/esm.js", "export const k = 1;\n");
    Loaded r2 = load(nested.write("main.js", "import { k } from 'lib';\n"));
    CHECK_FALSE(r2.ok);
    CHECK(contains(r2.errors, "CONDITIONAL object"));
}

TEST_CASE("an exports pattern and a fallback array are refused by name") {
    Sandbox box("bare_pattern");
    box.write("node_modules/lib/package.json", "{ \"exports\": { \"./*\": \"./src/*.js\" } }\n");
    Loaded r = load(box.write("main.js", "import { k } from 'lib/thing';\n"));
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "PATTERN key"));

    Sandbox arr("bare_fallback");
    arr.write("node_modules/lib/package.json",
              "{ \"exports\": { \".\": [\"./a.js\", \"./b.js\"] } }\n");
    Loaded r2 = load(arr.write("main.js", "import { k } from 'lib';\n"));
    CHECK_FALSE(r2.ok);
    CHECK(contains(r2.errors, "FALLBACK ARRAY"));
}

TEST_CASE("a package.json that will not parse is a named error") {
    Sandbox box("bare_badjson");
    box.write("node_modules/lib/package.json", "{ \"main\": 'index.js' }\n");
    Loaded r = load(box.write("main.js", "import x from 'lib';\n"));
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "is not valid JSON"));
}

TEST_CASE("a package.json with neither exports nor main is a named error") {
    // Not a fall back to index.js: that is a file the program never named, and
    // it exists here to prove the fallback is refused rather than absent.
    Sandbox box("bare_noentry");
    box.write("node_modules/lib/package.json", "{ \"name\": \"lib\" }\n");
    box.write("node_modules/lib/index.js", "export const k = 1;\n");
    Loaded r = load(box.write("main.js", "import { k } from 'lib';\n"));
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "neither \"exports\" nor \"main\""));
    CHECK(contains(r.errors, "does not fall back to index.js"));
}

TEST_CASE("a main that names no file is a named error, and two candidates name both") {
    Sandbox box("bare_nofile");
    box.write("node_modules/lib/package.json", "{ \"main\": \"./nope.js\" }\n");
    Loaded r = load(box.write("main.js", "import x from 'lib';\n"));
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "is no file"));
    CHECK(contains(r.errors, "does not guess an extension or a directory index"));

    // The ambiguity itself: `./lib` could be `lib.js` or `lib/index.js`, and a
    // precedence rule would silently compile one of the two.
    Sandbox amb("bare_ambiguous");
    amb.write("node_modules/lib/package.json", "{ \"main\": \"./thing\" }\n");
    amb.write("node_modules/lib/thing.js", "export const k = 1;\n");
    amb.write("node_modules/lib/thing/index.js", "export const k = 2;\n");
    Loaded r2 = load(amb.write("main.js", "import { k } from 'lib';\n"));
    CHECK_FALSE(r2.ok);
    CHECK(contains(r2.errors, "ambiguous which file was meant"));
    CHECK(contains(r2.errors, "thing.js"));
    CHECK(contains(r2.errors, "index.js"));
}

TEST_CASE("an absolute path, a URL and a `#` import are each refused by their own name") {
    Sandbox box("bare_kinds");
    Loaded abs = load(box.write("a.js", "import x from '/etc/passwd.js';\n"));
    CHECK_FALSE(abs.ok);
    CHECK(contains(abs.errors, "an absolute path"));

    Loaded url = load(box.write("b.js", "import x from 'https://cdn.example/m.js';\n"));
    CHECK_FALSE(url.ok);
    CHECK(contains(url.errors, "a URL"));

    Loaded builtin = load(box.write("c.js", "import x from 'node:fs';\n"));
    CHECK_FALSE(builtin.ok);
    CHECK(contains(builtin.errors, "URL scheme"));

    Loaded imports = load(box.write("d.js", "import x from '#internal';\n"));
    CHECK_FALSE(imports.ok);
    CHECK(contains(imports.errors, "\"imports\" map"));
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

// ---- the two writes, which are not one question ----------------------------
//
// `ns.k = 2` and `k = 2` were once the same refusal here, and that is why
// `cases/module_namespace_object` could not be built at all. They are different
// operations: one writes a PROPERTY of an ordinary object value, which nothing
// at compile time can decide (10.4.6.9 makes it a runtime TypeError); the other
// writes the immutable import BINDING, which linking has already renamed to the
// exporting module's own slot, so letting it through would store there and
// report nothing.

TEST_CASE("a property write through a namespace local LINKS, and is the runtime's question") {
    Sandbox box("nswrite");
    box.write("lib.js", "export let k = 1;\n");
    const std::string entry =
        box.write("main.js", "import * as ns from './lib.js';\nns.k = 2;\n");
    Loaded r = load(entry);
    REQUIRE_MESSAGE(r.ok, r.errors);
    // The namespace is the exotic object and not a plain literal, which is what
    // gives the runtime somewhere to refuse the write from.
    CHECK(contains(r.dump, "(module-namespace"));
}

TEST_CASE("an assignment to an import binding is refused by name") {
    Sandbox box("importwrite");
    box.write("lib.js", "export let k = 1;\n");
    Loaded named = load(box.write("main.js", "import { k } from './lib.js';\nk = 2;\n"));
    CHECK_FALSE(named.ok);
    CHECK(contains(named.errors, "cannot assign to 'k'"));
    CHECK(contains(named.errors, "import binding"));

    // `export let` and not `export const` on purpose: the exporting module may
    // still assign, so nothing about `k`'s own declaration says immutable. What
    // makes it immutable is that the importer's `k` is an IMPORT.
    Loaded compound = load(box.write("compound.js", "import { k } from './lib.js';\nk += 1;\n"));
    CHECK_FALSE(compound.ok);
    CHECK(contains(compound.errors, "cannot assign to 'k'"));

    Loaded update = load(box.write("update.js", "import { k } from './lib.js';\nk++;\n"));
    CHECK_FALSE(update.ok);
    CHECK(contains(update.errors, "cannot assign to 'k'"));

    Loaded destructured =
        load(box.write("destructure.js", "import { k } from './lib.js';\n[k] = [2];\n"));
    CHECK_FALSE(destructured.ok);
    CHECK(contains(destructured.errors, "cannot assign to 'k'"));

    // `for (k of [1])` is not here because the parser refuses an iteration head
    // that assigns an existing binding, whatever the binding is; the renamer
    // covers it anyway, for the day that head lands.

    // The NAMESPACE local is an import binding too, so rebinding it is the same
    // error — and this is the line that shows `ns = x` and `ns.k = x` are
    // separated by which one they write, not by whether a namespace is involved.
    Loaded rebind =
        load(box.write("rebind.js", "import * as ns from './lib.js';\nns = 5;\n"));
    CHECK_FALSE(rebind.ok);
    CHECK(contains(rebind.errors, "cannot assign to 'ns'"));

    // A local of the same name inside a function shadows the import and is an
    // ordinary variable: refusing there would reject a correct program.
    Loaded shadowed = load(box.write("shadow.js",
                                     "import { k } from './lib.js';\n"
                                     "function f() { let k = 1; k = 2; return k; }\n"
                                     "console.log(f());\n"));
    REQUIRE_MESSAGE(shadowed.ok, shadowed.errors);
}

TEST_CASE("a file with an import or an export is strict whether it says so or not") {
    // ECMA-262 11.2.2. Without it `ns.z = 5` is a sloppy assignment, which
    // 10.4.6.9 refuses and 13.15.2 then DISCARDS — three lines of output where
    // the second is wrong and nothing says so.
    Sandbox box("alwaysmodule");
    box.write("lib.js", "export let k = 1;\n");
    Loaded r = load(box.write("main.js", "import { k } from './lib.js';\nconsole.log(k);\n"));
    REQUIRE_MESSAGE(r.ok, r.errors);
    CHECK(contains(r.dump, "strict"));

    // A file with neither is a Script, and a Script's mode is its own business
    // — which is what keeps every single-file oracle case compiling as it did.
    Loaded script = load(box.write("script.js", "let x = 1;\nconsole.log(x);\n"));
    REQUIRE_MESSAGE(script.ok, script.errors);
    CHECK_FALSE(contains(script.dump, "strict"));
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

TEST_CASE("an entry module with strict early error is diagnosed as strict") {
    Sandbox box("entrystrict");
    const std::string entry =
        box.write("main.js", "export function f(a, a) {}\n");
    Loaded r = load(entry);
    CHECK_FALSE(r.ok);
    CHECK(contains(r.errors, "duplicate parameter name 'a'"));
}

TEST_CASE("virtual module root mapping resolves prefix imports") {
    Sandbox box("modroot");
    std::string libFile = box.write("shared/lib/math.js", "export function add(a, b) { return a + b; }\n");
    std::filesystem::path libDir = std::filesystem::path(libFile).parent_path();
    const std::string entry =
        box.write("app/main.js", "import { add } from '/lib/math.js';\nconsole.log(add(1, 2));\n");

    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions options;
    options.moduleRoots.push_back({"/lib", libDir});
    auto mod = modules::loadProgram(entry, sources, diags, options);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod != nullptr);
    const std::string dump = ast::dump(*mod);
    CHECK(contains(dump, "(function mod1.add"));
}

TEST_CASE("module root exact-prefix match against a single file target resolves without trailing slash") {
    Sandbox box("exact_root");
    std::string threeFile = box.write("vendor/build/three.module.js", "export const REVISION = '185';\n");
    const std::string entry =
        box.write("app/main.js", "import { REVISION } from 'three';\nconsole.log(REVISION);\n");

    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions options;
    options.moduleRoots.push_back({"three", threeFile});
    auto mod = modules::loadProgram(entry, sources, diags, options);
    REQUIRE_MESSAGE(mod != nullptr, diags.render(sources));
    const std::string dump = ast::dump(*mod);
    CHECK(contains(dump, "(const mod1.REVISION"));
}

TEST_CASE("module root matches longest prefix when file and directory roots co-exist") {
    Sandbox box("multi_root");
    std::string threeFile = box.write("three/build/three.module.js", "export const REVISION = '185';\n");
    std::string controlsFile =
        box.write("three/examples/jsm/controls/OrbitControls.js", "export class OrbitControls {}\n");
    std::filesystem::path jsmDir = std::filesystem::path(controlsFile).parent_path().parent_path();

    const std::string entry = box.write(
        "app/main.js",
        "import { REVISION } from 'three';\n"
        "import { OrbitControls } from 'three/addons/controls/OrbitControls.js';\n"
        "console.log(REVISION, OrbitControls);\n");

    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions options;
    // Order in options should not matter: longest prefix match takes precedence
    options.moduleRoots.push_back({"three", threeFile});
    options.moduleRoots.push_back({"three/addons/", jsmDir});

    auto mod = modules::loadProgram(entry, sources, diags, options);
    REQUIRE_MESSAGE(mod != nullptr, diags.render(sources));
    const std::string dump = ast::dump(*mod);
    CHECK(contains(dump, "(const mod1.REVISION"));
    CHECK(contains(dump, "(class mod2.OrbitControls"));
}

TEST_CASE("module root prefix does not match unrelated package with shared prefix") {
    Sandbox box("prefix_boundary");
    std::string threeFile = box.write("three/build/three.module.js", "export const REVISION = '185';\n");
    const std::string entry = box.write("app/main.js", "import { foo } from 'three-stdlib';\n");

    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions options;
    options.moduleRoots.push_back({"three", threeFile});

    auto mod = modules::loadProgram(entry, sources, diags, options);
    CHECK(mod == nullptr);
    CHECK(diags.hasErrors());
    std::string err = diags.render(sources);
    // Should fall through to node_modules lookup rather than claiming three.module.js/-stdlib is no file
    CHECK(contains(err, "no package named \"three-stdlib\""));
}

TEST_CASE("loadImportMap resolves relative targets against import map file directory") {
    Sandbox box("import_map_unit");
    std::string threeFile = box.write("vendor/build/three.module.js", "export const REVISION = '185';\n");
    std::string controlsFile =
        box.write("vendor/examples/jsm/controls/OrbitControls.js", "export class OrbitControls {}\n");
    std::string mapFile = box.write(
        "config/importmap.json",
        "{\n"
        "  \"imports\": {\n"
        "    \"three\": \"../vendor/build/three.module.js\",\n"
        "    \"three/addons/\": \"../vendor/examples/jsm/\"\n"
        "  }\n"
        "}\n");

    std::vector<modules::ModuleRoot> roots;
    std::string err;
    bool ok = modules::loadImportMap(mapFile, roots, err);
    REQUIRE_MESSAGE(ok, err);
    REQUIRE(roots.size() == 2);

    std::error_code ec;
    std::filesystem::path expectedThree = std::filesystem::weakly_canonical(threeFile, ec);
    std::filesystem::path expectedJsm =
        std::filesystem::weakly_canonical(std::filesystem::path(controlsFile).parent_path().parent_path(), ec);

    CHECK(roots[0].prefix == "three");
    CHECK(roots[0].target == expectedThree);
    CHECK(roots[1].prefix == "three/addons/");
    CHECK(roots[1].target == expectedJsm);
}

TEST_CASE("loadImportMap handles error cases") {
    Sandbox box("import_map_errors");

    // Non-existent file
    std::vector<modules::ModuleRoot> roots;
    std::string err;
    CHECK_FALSE(modules::loadImportMap(box.write("nonexistent.json", "").substr(0, 5) + "_nope.json", roots, err));
    CHECK(contains(err, "cannot read import map"));

    // Invalid JSON
    std::string badJson = box.write("bad.json", "{ imports: invalid }");
    CHECK_FALSE(modules::loadImportMap(badJson, roots, err));
    CHECK(contains(err, "is not valid JSON"));

    // Top-level not an object
    std::string arrayJson = box.write("array.json", "[\"imports\"]");
    CHECK_FALSE(modules::loadImportMap(arrayJson, roots, err));
    CHECK(contains(err, "is not a JSON object"));

    // imports not an object
    std::string nonObjImports = box.write("non_obj_imports.json", "{\"imports\": \"string\"}");
    CHECK_FALSE(modules::loadImportMap(nonObjImports, roots, err));
    CHECK(contains(err, "\"imports\""));
    CHECK(contains(err, "must be a JSON object"));

    // mapping target not a string
    std::string nonStrTarget = box.write("non_str_target.json", "{\"imports\": {\"three\": 123}}");
    CHECK_FALSE(modules::loadImportMap(nonStrTarget, roots, err));
    CHECK(contains(err, "must be a string"));
}

TEST_CASE("loadProgram with importMapPath integrates resolution end-to-end") {
    Sandbox box("import_map_e2e");
    box.write("libs/three.module.js", "export const V = 185;\n");
    box.write("libs/addons/controls/OrbitControls.js", "export function orbit() { return 100; }\n");
    std::string mapFile = box.write(
        "importmap.json",
        "{\n"
        "  \"imports\": {\n"
        "    \"three\": \"./libs/three.module.js\",\n"
        "    \"three/addons/\": \"./libs/addons/\"\n"
        "  }\n"
        "}\n");

    std::string entry = box.write(
        "src/main.js",
        "import { V } from 'three';\n"
        "import { orbit } from 'three/addons/controls/OrbitControls.js';\n"
        "console.log(V, orbit());\n");

    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions options;
    options.importMapPath = mapFile;

    auto mod = modules::loadProgram(entry, sources, diags, options);
    REQUIRE_MESSAGE(mod != nullptr, diags.render(sources));
    const std::string dump = ast::dump(*mod);
    CHECK(contains(dump, "(const mod1.V"));
    CHECK(contains(dump, "(function mod2.orbit"));
}


