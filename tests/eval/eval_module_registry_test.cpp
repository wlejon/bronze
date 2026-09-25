// Module identity ACROSS compilation units in one realm.
//
// A bronze unit is a whole flattened graph, so compiling the same file into a
// second unit used to run its top level a second time, in slots the first unit
// could not see. A host that compiles a page and then compiles a script against
// that page — a document and its headless test driver — means one instance of
// the page's modules, the way one module map per context meant it. That is what
// `EvalOptions::moduleRegistry` buys, and this file is what says it works.
//
// Each case writes its fixture to a directory of its own: the cases share one
// process and one realm, and a registry entry is keyed by path.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../test_temp_dir.h"
#include "embed/embed.h"
#include "eval/eval.h"
#include "runtime/module_registry.h"

using namespace bronze;
using namespace bronze::eval;

namespace {

std::filesystem::path makeDir(const char* name) {
    std::filesystem::path dir = bronze_test::tempDir() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

EvalOptions unitOptions(const std::filesystem::path& dir, const char* entry) {
    EvalOptions opts;
    opts.filename = (dir / entry).string();
    opts.entryResolvesAs = dir / entry;
    opts.moduleRegistry = true;
    return opts;
}

}  // namespace

TEST_CASE("a second unit binds the first unit's module instance") {
    const std::filesystem::path dir = makeDir("bronze_modreg_shared");
    // Top-level state a second evaluation would visibly duplicate: the id is
    // minted once per evaluation of the file, and the counter is the instance
    // the two units must agree about.
    writeFile(dir / "counter.js",
              "export const id = { n: 0 };\n"
              "export function bump() { return ++id.n; }\n");

    EvalOptions first = unitOptions(dir, "page.js");
    embed::CallResult r1 =
        evalScript("import { bump } from './counter.js'; bump(); bump();", first);
    REQUIRE(!r1.thrown);
    CHECK(r1.value.asNumber() == 2.0);

    // The second unit: a different entry, the same specifier. Without a
    // registry it would evaluate counter.js again and see 1.
    EvalOptions second = unitOptions(dir, "driver.js");
    embed::CallResult r2 = evalScript("import { bump } from './counter.js'; bump();", second);
    REQUIRE(!r2.thrown);
    CHECK(r2.value.asNumber() == 3.0);

    // Object IDENTITY across the seam, which is the property the tests that
    // import an app's modules actually depend on. One unit records the object
    // it imported; a third compares.
    embed::CallResult record = evalScript(
        "import { id } from './counter.js'; globalThis.__modregSeen = id; id.n", second);
    REQUIRE(!record.thrown);
    CHECK(record.value.asNumber() == 3.0);

    embed::CallResult compare =
        evalScript("import { id } from './counter.js'; globalThis.__modregSeen === id",
                   unitOptions(dir, "third.js"));
    REQUIRE(!compare.thrown);
    CHECK(compare.value.asBool());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("the top level of a shared module runs exactly once") {
    const std::filesystem::path dir = makeDir("bronze_modreg_once");
    writeFile(dir / "side.js",
              "globalThis.__modregEvals = (globalThis.__modregEvals || 0) + 1;\n"
              "export const marker = globalThis.__modregEvals;\n");

    embed::CallResult r1 =
        evalScript("import { marker } from './side.js'; marker", unitOptions(dir, "a.js"));
    REQUIRE(!r1.thrown);
    CHECK(r1.value.asNumber() == 1.0);

    embed::CallResult r2 =
        evalScript("import { marker } from './side.js'; marker", unitOptions(dir, "b.js"));
    REQUIRE(!r2.thrown);
    CHECK(r2.value.asNumber() == 1.0);

    embed::CallResult count = evalScript("globalThis.__modregEvals", EvalOptions{});
    REQUIRE(!count.thrown);
    CHECK(count.value.asNumber() == 1.0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a transitive dependency is shared too, and a new one is not") {
    const std::filesystem::path dir = makeDir("bronze_modreg_deep");
    writeFile(dir / "leaf.js", "export const box = { hits: 0 };\n");
    writeFile(dir / "mid.js",
              "import { box } from './leaf.js';\n"
              "export function hit() { return ++box.hits; }\n");
    writeFile(dir / "fresh.js",
              "import { box } from './leaf.js';\n"
              "export function peek() { return box.hits; }\n");

    embed::CallResult r1 =
        evalScript("import { hit } from './mid.js'; hit()", unitOptions(dir, "page.js"));
    REQUIRE(!r1.thrown);
    CHECK(r1.value.asNumber() == 1.0);

    // `fresh.js` was never compiled before, so this unit evaluates it — but the
    // `leaf.js` it imports IS published, so the two see one box.
    embed::CallResult r2 = evalScript(
        "import { peek } from './fresh.js'; import { hit } from './mid.js'; hit(); peek()",
        unitOptions(dir, "driver.js"));
    REQUIRE(!r2.thrown);
    CHECK(r2.value.asNumber() == 2.0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a namespace import and a default export cross the seam") {
    const std::filesystem::path dir = makeDir("bronze_modreg_ns");
    writeFile(dir / "thing.js",
              "export default class Thing { constructor() { this.tag = 'one'; } }\n"
              "export const NAMES = ['a', 'b'];\n"
              "export function label() { return 'L'; }\n");

    embed::CallResult r1 = evalScript(
        "import Thing, { NAMES } from './thing.js'; globalThis.__modregThing = Thing; NAMES.length",
        unitOptions(dir, "page.js"));
    REQUIRE(!r1.thrown);
    CHECK(r1.value.asNumber() == 2.0);

    // `import * as ns` over a module whose instance the registry holds: the
    // importing unit declares the namespace, built from the bindings the
    // registry lookup produced.
    embed::CallResult r2 = evalScript(
        "import * as ns from './thing.js';\n"
        "ns.default === globalThis.__modregThing && ns.label() === 'L' && ns.NAMES.length === 2",
        unitOptions(dir, "driver.js"));
    REQUIRE(!r2.thrown);
    CHECK(r2.value.asBool());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("an import across the seam is a live binding, not a snapshot") {
    const std::filesystem::path dir = makeDir("bronze_modreg_live");
    writeFile(dir / "live.js",
              "export let y = 1;\n"
              "export let c = null;\n"
              "export function bump() { y++; }\n"
              "export function rebuild() { c = { n: (c ? c.n : 0) + 1 }; }\n"
              "export function readC() { return c; }\n"
              "export function who() { return this; }\n"
              "export { y as alsoY };\n");

    embed::CallResult page = evalScript(
        "import { rebuild } from './live.js'; rebuild(); 0",
        unitOptions(dir, "page.js"));
    REQUIRE(!page.thrown);

    // Every write below happens in the exporting module's instance, after the
    // driver's bindings exist. A snapshot would still say 1 / the first `c`.
    embed::CallResult driver = evalScript(
        "import { y, alsoY, c, bump, rebuild, readC, who } from './live.js';\n"
        "import * as ns from './live.js';\n"
        "const out = [];\n"
        "bump();\n"
        "out.push(y === 2, alsoY === 2, ns.y === 2);\n"
        "rebuild();\n"
        "out.push(c === readC(), c.n === 2, ns.c === c);\n"
        "out.push(who() === undefined);\n"
        "const f = () => y; bump(); out.push(f() === 3);\n"
        "globalThis.__modregLiveNs = ns;\n"
        "out.join(',')",
        unitOptions(dir, "driver.js"));
    INFO((driver.thrown ? embed::toUtf8(driver.value) : std::string()));
    REQUIRE(!driver.thrown);
    CHECK(embed::toUtf8(driver.value) == "true,true,true,true,true,true,true,true");

    // A namespace import of an instance another unit made IS that instance's
    // published namespace, so two later units see one object.
    embed::CallResult third = evalScript(
        "import * as ns from './live.js'; ns === globalThis.__modregLiveNs && ns.y === 3",
        unitOptions(dir, "third.js"));
    REQUIRE(!third.thrown);
    CHECK(third.value.asBool());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("publishEntry: a module-file entry is the instance a later unit imports") {
    const std::filesystem::path dir = makeDir("bronze_modreg_entry");
    std::filesystem::create_directories(dir / "sub");
    writeFile(dir / "dep.js",
              "globalThis.__modregDepRuns = (globalThis.__modregDepRuns || 0) + 1;\n"
              "export const d = 1;\n");
    const std::string mainSrc =
        "import { d } from './dep.js';\n"
        "globalThis.__modregMainRuns = (globalThis.__modregMainRuns || 0) + 1;\n"
        "export let state = d;\n"
        "export function setState(v) { state = v; }\n";
    writeFile(dir / "main.js", mainSrc);

    // As a host hands a `<script type="module" src>` over: the file's text, the
    // file's path — spelled un-canonically, as a host's own path join may.
    EvalOptions page;
    page.filename = (dir / "sub" / ".." / "main.js").string();
    page.entryResolvesAs = dir / "main.js";
    page.moduleRegistry = true;
    page.publishEntry = true;
    embed::CallResult r1 = evalScript(mainSrc, page);
    INFO((r1.thrown ? embed::toUtf8(r1.value) : std::string()));
    REQUIRE(!r1.thrown);

    embed::CallResult r2 = evalScript(
        "import { state, setState } from './main.js'; setState(5); state",
        unitOptions(dir, "driver.js"));
    REQUIRE(!r2.thrown);
    CHECK(r2.value.asNumber() == 5.0);

    embed::CallResult runs =
        evalScript("globalThis.__modregMainRuns * 10 + globalThis.__modregDepRuns", EvalOptions{});
    REQUIRE(!runs.thrown);
    CHECK(runs.value.asNumber() == 11.0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("the registry is off unless the host asks, and the entry is never published") {
    const std::filesystem::path dir = makeDir("bronze_modreg_off");
    writeFile(dir / "twice.js",
              "globalThis.__modregOff = (globalThis.__modregOff || 0) + 1;\n"
              "export const n = globalThis.__modregOff;\n");

    EvalOptions plain;
    plain.filename = (dir / "a.js").string();
    plain.entryResolvesAs = dir / "a.js";
    embed::CallResult r1 = evalScript("import { n } from './twice.js'; n", plain);
    REQUIRE(!r1.thrown);
    CHECK(r1.value.asNumber() == 1.0);

    plain.filename = (dir / "b.js").string();
    plain.entryResolvesAs = dir / "b.js";
    embed::CallResult r2 = evalScript("import { n } from './twice.js'; n", plain);
    REQUIRE(!r2.thrown);
    // Two units, two evaluations — the behaviour a standalone program keeps.
    CHECK(r2.value.asNumber() == 2.0);

    // An ENTRY is the program being run, not an instance: running the same
    // file twice runs it twice even with the registry on.
    const std::filesystem::path entry = dir / "entry.js";
    writeFile(entry, "globalThis.__modregEntry = (globalThis.__modregEntry || 0) + 1;\n");
    EvalOptions on;
    on.moduleRegistry = true;
    embed::CallResult e1 = evalFile(entry.string(), on);
    REQUIRE(!e1.thrown);
    embed::CallResult e2 = evalFile(entry.string(), on);
    REQUIRE(!e2.thrown);
    embed::CallResult ran = evalScript("globalThis.__modregEntry", EvalOptions{});
    REQUIRE(!ran.thrown);
    CHECK(ran.value.asNumber() == 2.0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("rtModuleRegistryPaths lists what the units published") {
    const std::filesystem::path dir = makeDir("bronze_modreg_paths");
    writeFile(dir / "listed.js", "export const v = 1;\n");

    embed::CallResult r =
        evalScript("import { v } from './listed.js'; v", unitOptions(dir, "page.js"));
    REQUIRE(!r.thrown);

    const std::string key = std::filesystem::weakly_canonical(dir / "listed.js").generic_string();
    bool found = false;
    for (const std::string& path : runtime::rtModuleRegistryPaths()) {
        if (path == key) found = true;
    }
    CHECK(found);

    // And the entry is not in it.
    const std::string entryKey =
        std::filesystem::weakly_canonical(dir / "page.js").generic_string();
    for (const std::string& path : runtime::rtModuleRegistryPaths()) {
        CHECK(path != entryKey);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
