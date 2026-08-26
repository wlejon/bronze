// THE PIN CENSUS, end to end (src/runtime/pin_census.h, stage C1).
//
// Beside `pin_barrier_test.cpp` and for the same reason: what is asserted is a
// claim about a program COMPILED, RUN and then COMPILED AGAINST ITS OWN OUTPUT,
// and the composition root that does all three is the driver.
//
// Three kinds of assertion, and the third is the one that matters:
//
//   - the SHAPE, off `runIl`: which sites the census creates, and — as hard —
//     which it does not. A site for a parameter the closure parameter proof
//     already typed would be the overlap stage E4's HANDOFF (c) warned about.
//   - the CONTENT, off a census build and its run: which lines the manifest
//     holds, and which candidates it refuses and why.
//   - the ROUND TRIP: the emitted manifest fed straight back to `--pins`,
//     producing a program that prints what the unpinned one printed. A census
//     whose output does not compile is a census that has produced nothing.

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/driver.h"
#include "types/pins.h"

namespace {

std::filesystem::path workDir() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bronze_pin_census";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// The IL of `source` compiled with the census on, which is where the SITE TABLE
// is readable without running anything.
std::string censusIl(const std::string& name, const std::string& source) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    write(js, source);
    std::string il;
    const int status = bronze::cli::runIl(js.string(), &il, /*infer=*/true,
                                          /*hostGlobalsPath=*/{}, /*moduleRoots=*/{},
                                          /*importMapPath=*/{}, /*inferStats=*/false,
                                          /*assumeNoBigInt=*/false, /*pinsPath=*/{},
                                          (dir / (name + ".pins")).string());
    REQUIRE(status == 0);
    return il;
}

// Every non-comment entry of a manifest, in file order. The provenance comment
// is stripped: it carries observation counts, which are a property of the run
// and not of the claim.
std::vector<std::string> entriesOf(const std::string& manifest) {
    std::vector<std::string> out;
    std::istringstream lines(manifest);
    std::string line;
    while (std::getline(lines, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

bool holds(const std::vector<std::string>& entries, const std::string& what) {
    for (const auto& e : entries) {
        if (e == what) return true;
    }
    return false;
}

#if BRONZE_WITH_LLVM
std::string runOutput(const std::filesystem::path& exePath) {
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(exePath.string().c_str(), "r");
#else
    FILE* pipe = popen(exePath.string().c_str(), "r");
#endif
    if (!pipe) return result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result += buffer;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

struct CensusRun {
    std::string manifest;
    std::string output;
    std::filesystem::path manifestPath;
};

// Compile `source` with `--census`, run it, and hand back the manifest it wrote
// and everything it printed. The manifest is the artefact; the output is how a
// test knows the run actually happened.
CensusRun census(const std::string& name, const std::string& source) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    const std::filesystem::path exe = dir / (name + "_census.exe");
    const std::filesystem::path pins = dir / (name + ".pins");
    write(js, source);
    std::error_code ec;
    std::filesystem::remove(exe, ec);
    std::filesystem::remove(pins, ec);

    std::string err;
    const int status = bronze::cli::runBuild(
        js.string(), exe.string(), &err, /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr, /*moduleRoots=*/{},
        /*entrySymbol=*/{}, /*emitShared=*/false, /*retainFnSource=*/true, /*importMapPath=*/{},
        /*assumeNoBigInt=*/false, /*pinsPath=*/{}, pins.string());
    REQUIRE_MESSAGE(status == 0, err);
    CensusRun run;
    run.output = runOutput(exe);
    run.manifest = read(pins);
    run.manifestPath = pins;
    return run;
}

// The other half of the loop: build `source` against a manifest and run it.
std::string buildWithPins(const std::string& name, const std::string& source,
                          const std::filesystem::path& pins, bool allowObserved,
                          int* statusOut = nullptr, std::string* errOut = nullptr) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    const std::filesystem::path exe = dir / (name + "_pinned.exe");
    write(js, source);
    std::error_code ec;
    std::filesystem::remove(exe, ec);
    std::string err;
    const int status = bronze::cli::runBuild(
        js.string(), exe.string(), &err, /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr, /*moduleRoots=*/{},
        /*entrySymbol=*/{}, /*emitShared=*/false, /*retainFnSource=*/true, /*importMapPath=*/{},
        /*assumeNoBigInt=*/false, pins.string(), /*censusOutPath=*/{}, allowObserved);
    if (statusOut) *statusOut = status;
    if (errOut) *errOut = err;
    if (status != 0) return {};
    return runOutput(exe);
}
#endif

// The factory-closure shape the whole campaign is aimed at: hot state in
// captured bindings, one closure handed OUT, and the parameter of that closure
// the E4 proof provably cannot reach (bench/env_slot_kernel.js in miniature).
const char* kFactory =
    "function makeState() {\n"
    "  let current = 0;\n"
    "  let changes = 0;\n"
    "  function set(v) { if (v !== current) { current = v; changes = changes + 1; } }\n"
    "  function hits() { return changes; }\n"
    "  function render(n) {\n"
    "    for (let i = 0; i < n; i++) set(i & 3);\n"
    "    return hits();\n"
    "  }\n"
    "  return render;\n"
    "}\n"
    "console.log('checksum ' + makeState()(64));\n";

// The DYNAMIC-RECEIVER GAP, which is B1's negative 1 (types/pins.h). Two
// classes with the same field name reach `putBlind`, so inference's join types
// `o` dynamic and the store there resolves to no class at all: it carries no
// barrier, while `v.x` read as a `Vec` elsewhere still spends the claim. Two
// classes and not one, because with a single call site the signature join types
// `o` a `Vec` and the store is held to the promise like every other.
const char* kBlindStore =
    "class Vec { constructor(x) { this.x = x; } }\n"
    "class Tag { constructor(x) { this.x = x; } }\n"
    "function putBlind(o, y) { o.x = y; }\n"
    "const v = new Vec(1);\n"
    "putBlind(v, 2);\n"
    "putBlind(new Tag(5), 3);\n"
    "console.log(v.x);\n";

}  // namespace

// ---- the sites, before anything runs ---------------------------------------

TEST_CASE("the census sites the slot a fixpoint refused and not the one it proved") {
    const std::string il = censusIl("sites_slots", kFactory);
    // `current` is written from a PARAMETER, which nothing proves.
    CHECK(il.find("env-slot \"function makeState.current\"") != std::string::npos);
    // `changes` is `changes + 1` at every write, which the greatest fixpoint in
    // lower_scope.cpp proves with no manifest at all. A site for it would be
    // the overlap stage E4's HANDOFF (c) told the census to avoid.
    CHECK(il.find("function makeState.changes") == std::string::npos);
}

TEST_CASE("the census sites the ESCAPED closure's parameter, which the E4 proof cannot") {
    const std::string il = censusIl("sites_escaped", kFactory);
    // `render` is RETURNED, so `planClosureParamNumbers` refuses it: its call
    // sites are not in the factory's subtree. This is the one line of
    // bench/pins/env-slot-kernel.pins the proof leaves standing, and recovering
    // it is the census's reason to exist.
    CHECK(il.find("param \"param render(n)\"") != std::string::npos);
    // `set` is CALLED by the factory's own body, so the proof types its
    // parameter f64 and there is nothing left to observe.
    CHECK(il.find("param set(v)") == std::string::npos);
}

TEST_CASE("a return the body can fall off is refused by the site table, not by a run") {
    const std::string il = censusIl("sites_falloff",
                                    "function maybe(x) { if (x > 0) return 1; }\n"
                                    "console.log(maybe(1) + ' ' + maybe(-1));\n");
    // Every observation of this return would be a Number — the `return 1` is
    // the only one that executes — and the entry is still refused, because
    // falling off the end yields `undefined` and `applySignaturePins` would
    // reject the line. Reachability is a property of the PROGRAM and no run can
    // be asked about it, which is why the site table carries static refusals.
    CHECK(il.find("return \"return maybe\" x2 refused") != std::string::npos);
}

TEST_CASE("a store through a receiver inference cannot type registers the opaque row") {
    const std::string il =
        censusIl("sites_opaque", kBlindStore);
    CHECK(il.find("field \"Vec.x\"") != std::string::npos);
    // B1's negative 1, as a row: `o` is dynamic, so this store carries no
    // barrier and no entry for a field named `x` is fully enforced.
    CHECK(il.find("opaque-store \"x\"") != std::string::npos);
}

TEST_CASE("an array index is not a field and never registers an opaque row") {
    const std::string il = censusIl("sites_index",
                                    "const a = [1, 2, 3];\n"
                                    "a[0] = 9;\n"
                                    "console.log(a[0]);\n");
    // A manifest cannot spell `<Class>.0` at all, so a row for one could only
    // ever mark real entries `@observed` on evidence that means nothing.
    CHECK(il.find("opaque-store \"0\"") == std::string::npos);
}

#if BRONZE_WITH_LLVM

// ---- the manifest, and the loop closing ------------------------------------

TEST_CASE("the census writes the manifest a hand author would have written") {
    const CensusRun run = census("write_factory", kFactory);
    REQUIRE(run.output == "checksum 63\n");
    const auto entries = entriesOf(run.manifest);
    CHECK(holds(entries, "function makeState.current: number"));
    CHECK(holds(entries, "param render(n): number"));
    CHECK(holds(entries, "return render: number"));
    // No entry for the proved slot, because there is no site for it.
    for (const auto& e : entries) CHECK(e.find("makeState.changes") == std::string::npos);
}

TEST_CASE("the emitted manifest builds, and the program prints what it printed") {
    const CensusRun run = census("roundtrip", kFactory);
    REQUIRE(run.output == "checksum 63\n");
    int status = 1;
    std::string err;
    const std::string pinned = buildWithPins("roundtrip", kFactory, run.manifestPath,
                                             /*allowObserved=*/false, &status, &err);
    REQUIRE_MESSAGE(status == 0, err);
    CHECK(pinned == run.output);
}

TEST_CASE("a polymorphic site is not emitted, and the file says why") {
    const std::string src =
        "class Box { constructor(v) { this.v = v; } }\n"
        "const a = new Box(1);\n"
        "const b = new Box('two');\n"
        "console.log(a.v + '|' + b.v);\n";
    const CensusRun run = census("poly", src);
    REQUIRE(run.output == "1|two\n");
    const auto entries = entriesOf(run.manifest);
    for (const auto& e : entries) CHECK(e.find("Box.v") == std::string::npos);
    // The refusal is in the file as a comment, with the tally that produced it:
    // a census whose output is only its hits tells a reader nothing about the
    // claims it declined.
    CHECK(run.manifest.find("# refused Box.v (field): polymorphic:") != std::string::npos);
}

TEST_CASE("a name no manifest line can spell is refused, so the file still parses") {
    // An ACCESSOR lowers to an IL function named `Euler.set x` — a space in the
    // middle — and `param Euler.set x(value): number` is not a line the parser
    // accepts. This is the census's worst failure mode and the only one it can
    // have: not a wrong claim, which stage B1 catches, but a file the build
    // handed it REFUSES TO READ. It cost the three.js oracle nine entries.
    const std::string src =
        "class Euler {\n"
        "  constructor() { this._x = 0; }\n"
        "  set x(value) { this._x = value; }\n"
        "  get x() { return this._x; }\n"
        "}\n"
        "const e = new Euler();\n"
        "e.x = 4;\n"
        "console.log(e.x);\n";
    const CensusRun run = census("accessor", src);
    REQUIRE(run.output == "4\n");
    for (const auto& e : entriesOf(run.manifest)) CHECK(e.find("set x") == std::string::npos);
    CHECK(run.manifest.find("# refused param Euler.set x(value)") != std::string::npos);

    // The point of the refusal: what IS in the file is a file that builds.
    int status = 1;
    std::string err;
    const std::string pinned = buildWithPins("accessor", src, run.manifestPath,
                                             /*allowObserved=*/false, &status, &err);
    REQUIRE_MESSAGE(status == 0, err);
    CHECK(pinned == run.output);
}

TEST_CASE("two same-named functions make an owner ambiguous even as duplicates") {
    // Two factories each declare a nested `get` — two DISTINCT IL functions
    // with the SAME name (three.js's WebGLRenderLists/WebGLRenderStates pair,
    // one of them with a defaulted parameter). A `param get(x)` entry matches
    // both by spelling, and applied to the defaulted one it is a hard build
    // error. The ambiguity table must count functions, not names: a set of
    // names collapses the duplicates into an owner that looks unique.
    const std::string src =
        "function A() {\n"
        "  function get(x) { return x + 1; }\n"
        "  return get;\n"
        "}\n"
        "function B() {\n"
        "  function get(x = 5) { return x + 2; }\n"
        "  return get;\n"
        "}\n"
        "const a = A(), b = B();\n"
        "let s = a(1) + a(2);\n"
        "s += b();\n"
        "console.log(s);\n";
    const CensusRun run = census("dupname", src);
    REQUIRE(run.output == "12\n");
    for (const auto& e : entriesOf(run.manifest)) CHECK(e.find("param get") == std::string::npos);
    CHECK(run.manifest.find("# refused param get(x)") != std::string::npos);

    int status = 1;
    std::string err;
    const std::string pinned = buildWithPins("dupname", src, run.manifestPath,
                                             /*allowObserved=*/false, &status, &err);
    REQUIRE_MESSAGE(status == 0, err);
    CHECK(pinned == run.output);
}

TEST_CASE("a field only ever seen nullish is refused, not widened") {
    const std::string src =
        "class Mat { constructor() { this.planes = null; } }\n"
        "const m = new Mat();\n"
        "console.log(m.planes);\n";
    const CensusRun run = census("nullonly", src);
    REQUIRE(run.output == "null\n");
    const auto entries = entriesOf(run.manifest);
    for (const auto& e : entries) CHECK(e.find("Mat.planes") == std::string::npos);
    CHECK(run.manifest.find("only ever nullish") != std::string::npos);
}

TEST_CASE("a number-or-nullish field is emitted when both arms were seen") {
    const std::string src =
        "class Node { constructor(l) { this.limit = l; } }\n"
        "const a = new Node(0.5);\n"
        "const b = new Node(null);\n"
        "console.log(a.limit + '|' + b.limit);\n";
    const CensusRun run = census("nullish", src);
    REQUIRE(run.output == "0.5|null\n");
    CHECK(holds(entriesOf(run.manifest), "Node.limit: number-or-nullish"));
}

TEST_CASE("a field with an untypeable store is marked @observed and refused by default") {
    const std::string src = kBlindStore;
    const CensusRun run = census("observed", src);
    REQUIRE(run.output == "2\n");
    CHECK(holds(entriesOf(run.manifest), "Vec.x: number @observed"));

    // A default build REFUSES it, by name, because a violation through
    // `putBlind` would be silent rather than a TypeError (types/pins.h).
    int status = 0;
    std::string err;
    buildWithPins("observed", src, run.manifestPath, /*allowObserved=*/false, &status, &err);
    CHECK(status != 0);
    CHECK(err.find("@observed") != std::string::npos);

    // And accepts it when the invocation says so, which is the whole content of
    // the flag: taking back stage B1's guarantee for one entry, named in a file.
    status = 1;
    const std::string out = buildWithPins("observed", src, run.manifestPath,
                                          /*allowObserved=*/true, &status, &err);
    REQUIRE_MESSAGE(status == 0, err);
    CHECK(out == run.output);
}

// ---- the enforcement interlock ----------------------------------------------

TEST_CASE("a wrong inference is a TypeError naming the census's own line") {
    // The census run only ever passes numbers, so the manifest promises a
    // number. The SECOND program is the same code reached with a string — the
    // representative run was not representative — and stage B1 is what turns
    // that from a pointer read as a double into a diagnostic.
    const char* kReport =
        "function report(label, fn) {\n"
        "  try { fn(); console.log(label + ' NOTHROW'); }\n"
        "  catch (e) { console.log(label + ' ' + (e instanceof TypeError) + ' ' + e.message); }\n"
        "}\n";
    const std::string censusSrc =
        "function makeState() {\n"
        "  let current = 0;\n"
        "  function set(v) { current = v; }\n"
        "  function get() { return current; }\n"
        "  function render(n) { set(n); return get(); }\n"
        "  return render;\n"
        "}\n"
        "console.log('checksum ' + makeState()(7));\n";
    const CensusRun run = census("interlock", censusSrc);
    REQUIRE(run.output == "checksum 7\n");
    REQUIRE(holds(entriesOf(run.manifest), "function makeState.current: number"));

    const std::string violatingSrc = std::string(kReport) + censusSrc +
                                     "const r = makeState();\n"
                                     "report('slot', function () { r('boom'); });\n"
                                     "console.log('alive ' + r(3));\n";
    int status = 1;
    std::string err;
    const std::string out = buildWithPins("interlock_violate", violatingSrc, run.manifestPath,
                                          /*allowObserved=*/false, &status, &err);
    REQUIRE_MESSAGE(status == 0, err);
    if (bronze::types::pinBarriersEnabled()) {
        CHECK(out.find("slot true") != std::string::npos);
        // The message reads back as A LINE OF THE MANIFEST THE CENSUS WROTE,
        // which is what closes the loop from a throw in the field to the file
        // that caused it (stage B1's HANDOFF (d), item 2). WHICH line is not
        // asserted: `render`'s pinned parameter is contradicted at the boxed
        // wrapper, before the slot it would have been stored into, and either
        // barrier firing first is the same correct answer.
        const auto open = out.find("pin '");
        REQUIRE(open != std::string::npos);
        const auto close = out.find('\'', open + 5);
        REQUIRE(close != std::string::npos);
        const std::string named = out.substr(open + 5, close - open - 5);
        CHECK(holds(entriesOf(run.manifest), named));
    }
    // And the program is still running afterwards, whatever the seam says.
    CHECK(out.find("alive 3") != std::string::npos);
}

#endif  // BRONZE_WITH_LLVM
