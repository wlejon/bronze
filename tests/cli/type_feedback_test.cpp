// TYPE FEEDBACK, end to end (docs/type-feedback.md): a program compiled with
// `--feedback-out`, run, and its profile read back through the reader the
// consuming build will use.
//
// Beside pin_census_test.cpp for the same reason: the claim is about a program
// COMPILED and RUN, and the driver is the composition root that does both.
// What is asserted:
//
//   - the CONTENT: the tags a parameter saw, the two shapes and two callees of
//     a polymorphic method call, the callee of a dynamic call, the entry count;
//   - the FORMAT: the runtime's file round-trips through the reader byte for
//     byte, which is what holds the two writers together;
//   - the ABSENCE: a build without `--feedback-out` carries no record, and
//     `--feedback-in` alone changes no byte of the object.

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../test_temp_dir.h"
#include "cli/driver.h"
#include "opt/spec/profile.h"

namespace {

std::filesystem::path workDir() {
    std::filesystem::path dir = bronze_test::tempDir() / "bronze_type_feedback";
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

int build(const std::filesystem::path& js, const std::filesystem::path& out, std::string& err,
          bool emitObj, const std::string& census, const std::string& feedbackOut,
          const std::string& feedbackIn) {
    return bronze::cli::runBuild(
        js.string(), out.string(), &err, /*infer=*/true, /*timings=*/false, emitObj,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr, /*moduleRoots=*/{},
        /*entrySymbol=*/{}, /*emitShared=*/false, /*retainFnSource=*/true, /*importMapPath=*/{},
        /*pinsPath=*/{}, census, /*pinsAllowObserved=*/false,
        /*nativeManifestPath=*/{}, /*nativeLibPath=*/{}, /*entryResolvesAs=*/{},
        /*targetName=*/{}, /*publishModules=*/false, /*emitDebugInfo=*/false,
        /*specProfilePath=*/{}, /*wpoStats=*/false, /*wpoStatsFilter=*/{},
        /*wpoStatsPerSite=*/false, feedbackOut, feedbackIn);
}

// `sum` is reached only through an array, so no proof types its parameters,
// and it is called once with a string. `o.len()` meets two classes.
const char* const kProgram = R"JS(
class Vec { constructor(x, y) { this.x = x; this.y = y; } len() { return this.x * this.x + this.y * this.y; } }
class Pt { constructor(x) { this.x = x; } len() { return this.x; } }
function sum(a, b) { return a + b; }
const fns = [sum, function (a, b) { return a - b; }];
const objs = [];
for (let i = 0; i < 10; i++) objs.push(i % 2 ? new Vec(i, 1.5) : new Pt(i));
let acc = 0;
for (let i = 0; i < 100; i++) {
  const f = fns[0];
  acc = f(acc, i);
  const o = objs[i % 10];
  acc += o.len();
}
const g = fns[0];
console.log(acc, g("s", 1), __bronze_feedback_flush());
)JS";

const bronze::spec::FunctionFeedback* byName(const bronze::spec::TypeProfile& p, const std::string& name) {
    for (const auto& [key, fn] : p.functions()) {
        if (key.rfind(name + "#", 0) == 0) return &fn;
    }
    return nullptr;
}

bool hasName(const std::vector<std::pair<std::string, uint64_t>>& list, const std::string& prefix) {
    for (const auto& [name, count] : list) {
        if (name.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("type feedback: a run records tags, shapes and callees under stable keys") {
    const auto dir = workDir();
    const auto js = dir / "fb_prog.js";
    const auto exe = dir / "fb_prog.exe";
    const auto tfb = dir / "fb_prog.tfb";
    write(js, kProgram);
    std::error_code ec;
    std::filesystem::remove(tfb, ec);

    std::string err;
    REQUIRE_MESSAGE(build(js, exe, err, false, {}, tfb.string(), {}) == 0, err);
    const std::string output = runOutput(exe);
    // The flush answers true in an instrumented build.
    CHECK(output == "6912.5 s1 true\n");

    const std::string text = read(tfb);
    auto profile = bronze::spec::TypeProfile::parse(text, &err);
    REQUIRE_MESSAGE(profile.has_value(), err);
    CHECK(profile->serialize() == text);

    const auto* sum = byName(*profile, "sum");
    REQUIRE(sum != nullptr);
    CHECK(sum->entries == 101);
    bool sawStringParam = false;
    for (const auto& [ord, site] : sum->sites) {
        if (site.kind != bronze::il::FeedbackKind::Param) continue;
        if (site.tags[BRONZE_FEEDBACK_TAG_STRING] == 1 && site.tags[BRONZE_FEEDBACK_TAG_I32] != 0) {
            sawStringParam = true;
            CHECK_FALSE(site.monomorphic());
        }
    }
    CHECK(sawStringParam);

    const auto* main = byName(*profile, "main");
    REQUIRE(main != nullptr);
    CHECK(main->entries == 0);  // the entry point carries no entry site
    bool sawMethod = false;
    bool sawTarget = false;
    for (const auto& [ord, site] : main->sites) {
        if (site.kind == bronze::il::FeedbackKind::Method && site.total == 100) {
            sawMethod = true;
            CHECK(site.shapes.size() == 2);
            CHECK(hasName(site.shapes, "Pt{x}"));
            CHECK(hasName(site.shapes, "Vec{x,y}"));
            CHECK(site.targets.size() == 2);
            CHECK(hasName(site.targets, "len@"));
            CHECK(site.soleTarget().empty());
        }
        if (site.kind == bronze::il::FeedbackKind::Target && site.total == 100) {
            sawTarget = true;
            CHECK(site.soleTarget().rfind("sum@", 0) == 0);
            CHECK(site.tags[BRONZE_FEEDBACK_TAG_FUNCTION] == 100);
        }
    }
    CHECK(sawMethod);
    CHECK(sawTarget);

    // Each method body sees one receiver shape.
    for (const char* cls : {"Vec.len", "Pt.len"}) {
        const auto* fn = byName(*profile, cls);
        REQUIRE(fn != nullptr);
        CHECK(fn->entries == 50);
        for (const auto& [ord, site] : fn->sites) {
            if (site.kind == bronze::il::FeedbackKind::Shape) CHECK_FALSE(site.soleShape().empty());
        }
    }
}

TEST_CASE("type feedback: a build without --feedback-out carries nothing of it") {
    const auto dir = workDir();
    const auto js = dir / "fb_plain.js";
    write(js, kProgram);

    std::string il;
    REQUIRE(bronze::cli::runIl(js.string(), &il) == 0);
    CHECK(il.find("feedback.record") == std::string::npos);
    CHECK(il.find("\nfeedback ") == std::string::npos);
    std::string fbIl;
    REQUIRE(bronze::cli::runIl(js.string(), &fbIl, true, {}, {}, {}, false, {}, {}, false, {}, {},
                               (dir / "x.tfb").string()) == 0);
    CHECK(fbIl.find("feedback.record") != std::string::npos);

    // A profile none of whose functions the build has changes nothing: the
    // object is the same bytes.
    const auto tfb = dir / "fb_in.tfb";
    write(tfb, "bronze-type-feedback 1\nfn\tsum#0000000000000000\t3\nsite\t0\tentry\t3\t\n");
    std::string err;
    const auto plainObj = dir / "fb_plain.o";
    const auto inObj = dir / "fb_in.o";
    REQUIRE_MESSAGE(build(js, plainObj, err, true, {}, {}, {}) == 0, err);
    REQUIRE_MESSAGE(build(js, inObj, err, true, {}, {}, tfb.string()) == 0, err);
    CHECK(read(plainObj) == read(inObj));

    // The flush is a builtin either way; with nothing registered it writes
    // nothing and says so.
    const auto exe = dir / "fb_plain.exe";
    REQUIRE_MESSAGE(build(js, exe, err, false, {}, {}, {}) == 0, err);
    CHECK(runOutput(exe) == "6912.5 s1 false\n");
}

TEST_CASE("type feedback: refusals name the flag") {
    const auto dir = workDir();
    const auto js = dir / "fb_refuse.js";
    write(js, "console.log(1);\n");
    std::string err;
    CHECK(build(js, dir / "fb_refuse.exe", err, false, (dir / "c.pins").string(),
                (dir / "c.tfb").string(), {}) == 1);
    CHECK(err.find("--census") != std::string::npos);

    err.clear();
    const auto bad = dir / "fb_bad.tfb";
    write(bad, "bronze-type-feedback 1\nsite\t0\tvalue\t1\t\n");
    CHECK(build(js, dir / "fb_refuse.exe", err, false, {}, {}, bad.string()) == 1);
    CHECK(err.find("--feedback-in") != std::string::npos);
    CHECK(err.find("line 2") != std::string::npos);

    err.clear();
    CHECK(build(js, dir / "fb_refuse.exe", err, false, {}, {}, (dir / "missing.tfb").string()) == 1);
    CHECK(err.find("cannot read") != std::string::npos);
}

namespace {

// Every value kind through untyped truthiness sites: `if (v)`, `!v`, `v ? :`,
// the join of `a && b` and `a || b`, each reached through an array so no
// proof types it.
const char* const kTruthProgram = R"JS(
function t(v) { if (v) return 1; return 0; }
function n(v) { return !v ? 1 : 0; }
function c(v) { return v ? "y" : "n"; }
function a(v, w) { return v && w ? "B" : "N"; }
function o(v, w) { return (v || w) ? "O" : "Z"; }
const fns = [t, n, c, a, o];
const vals = [
  0, -0, 1, -1, 0.5, NaN, Infinity, 5e-324, 2 ** 31, 0 | 0, 7 | 0,
  0n, 1n, -1n, 2n ** 70n, "", "0", "a", true, false, null, undefined,
  {}, [], function () {}, Symbol("s"), new Boolean(false),
];
let out = "";
for (let r = 0; r < 2; r++) {
  for (const v of vals) {
    for (const f of fns) out += f(v, v);
    out += f4(v, false) + f4(v, null);
    out += " ";
  }
}
function f4(v, w) { return fns[3](v, w) + fns[4](v, w); }
console.log(out);
)JS";

// A profile whose every value site saw `tags` and nothing else.
bronze::spec::TypeProfile retagged(const bronze::spec::TypeProfile& real, const std::vector<int>& tags) {
    bronze::spec::TypeProfile out;
    for (const auto& [key, fn] : real.functions()) {
        bronze::spec::FunctionFeedback copy = fn;
        for (auto& [ord, site] : copy.sites) {
            using K = bronze::il::FeedbackKind;
            if (site.kind != K::Value && site.kind != K::Param && site.kind != K::This && site.kind != K::Result) {
                continue;
            }
            site.tags = {};
            site.total = 0;
            for (int tag : tags) {
                site.tags[tag] = 1000;
                site.total += 1000;
            }
        }
        out.put(std::move(copy));
    }
    return out;
}

}  // namespace

// A truthiness site hinted with the kinds a profile saw tests those first; a
// profile that saw other kinds than the run meets may only cost speed. Each
// build here takes a profile claiming every value site saw only some of
// Boolean / object / undefined / null and is fed every kind of value.
TEST_CASE("type feedback: a wrong truthiness profile changes no answer") {
    const auto dir = workDir();
    const auto js = dir / "fb_truth.js";
    const auto tfb = dir / "fb_truth.tfb";
    write(js, kTruthProgram);
    std::error_code ec;
    std::filesystem::remove(tfb, ec);
    std::string err;
    const auto plainExe = dir / "fb_truth_plain.exe";
    REQUIRE_MESSAGE(build(js, plainExe, err, false, {}, {}, {}) == 0, err);
    const std::string expected = runOutput(plainExe);
    REQUIRE(expected.size() > 100);

    const auto fbExe = dir / "fb_truth_fb.exe";
    REQUIRE_MESSAGE(build(js, fbExe, err, false, {}, tfb.string(), {}) == 0, err);
    CHECK(runOutput(fbExe) == expected);
    auto profile = bronze::spec::TypeProfile::parse(read(tfb), &err);
    REQUIRE_MESSAGE(profile.has_value(), err);

    const std::vector<std::vector<int>> mixes = {
        {BRONZE_FEEDBACK_TAG_BOOL},
        {BRONZE_FEEDBACK_TAG_OBJECT},
        {BRONZE_FEEDBACK_TAG_UNDEFINED},
        {BRONZE_FEEDBACK_TAG_NULL},
        {BRONZE_FEEDBACK_TAG_BOOL, BRONZE_FEEDBACK_TAG_UNDEFINED},
        {BRONZE_FEEDBACK_TAG_OBJECT, BRONZE_FEEDBACK_TAG_NULL, BRONZE_FEEDBACK_TAG_UNDEFINED},
        {BRONZE_FEEDBACK_TAG_BOOL, BRONZE_FEEDBACK_TAG_OBJECT, BRONZE_FEEDBACK_TAG_NULL,
         BRONZE_FEEDBACK_TAG_UNDEFINED},
    };
    for (size_t i = 0; i < mixes.size(); ++i) {
        CAPTURE(i);
        const bronze::spec::TypeProfile wrong = retagged(*profile, mixes[i]);
        const auto wrongTfb = dir / ("fb_truth_wrong" + std::to_string(i) + ".tfb");
        write(wrongTfb, wrong.serialize());
        const auto exe = dir / ("fb_truth_wrong" + std::to_string(i) + ".exe");
        REQUIRE_MESSAGE(build(js, exe, err, false, {}, {}, wrongTfb.string()) == 0, err);
        CHECK(runOutput(exe) == expected);
    }
}
