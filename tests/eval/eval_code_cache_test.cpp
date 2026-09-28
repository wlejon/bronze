// The on-disk code cache (EvalOptions::codeCacheDir, src/eval/code_cache.h):
// an unchanged program is served from the cache and runs the same, and every
// way the cache can be wrong — an edited source, a new file under a glob, a
// different compile input, a truncated or damaged entry — is a miss that
// compiles from source.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../test_temp_dir.h"
#include "embed/embed.h"
#include "eval/eval.h"

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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The one entry file a cache directory holds after one program was stored.
std::filesystem::path onlyEntry(const std::filesystem::path& cacheDir) {
    std::filesystem::path found;
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(cacheDir)) {
        if (e.path().extension() == ".bzc") {
            found = e.path();
            ++n;
        }
    }
    REQUIRE(n == 1);
    return found;
}

EvalOptions cached(const std::filesystem::path& cacheDir, const std::string& name) {
    EvalOptions opts;
    opts.filename = name;
    opts.hostGlobals = {"console"};
    opts.codeCacheDir = cacheDir.string();
    return opts;
}

double runNumber(std::unique_ptr<CompiledScript> script, const EvalOptions& opts) {
    REQUIRE(script->success);
    embed::CallResult r = runCompiledScript(std::move(script), opts);
    REQUIRE(!r.thrown);
    return r.value.asNumber();
}

// Closures, classes, a generator (whose body is lowered eagerly) and ordinary
// functions (lowered lazily on first call): the stored IL has to carry all of
// them.
constexpr const char* kProgram =
    "class Acc { constructor(n) { this.n = n; } add(k) { this.n += k; return this; } }\n"
    "function* evens(limit) { for (let i = 0; i < limit; i += 2) yield i; }\n"
    "function make(base) { return (x) => base * x; }\n"
    "let total = 0;\n"
    "for (const e of evens(10)) total += e;\n"
    "const acc = new Acc(total).add(make(3)(4));\n"
    "acc.n + `${'x'.repeat(2)}`.length;\n";
constexpr double kProgramValue = 20 + 12 + 2;

}  // namespace

TEST_CASE("code cache: a miss stores, a hit runs the same program") {
    const auto cacheDir = makeDir("bronze_cc_hit");
    const EvalOptions opts = cached(cacheDir, "<cc-hit>");

    auto first = compileScript(kProgram, opts);
    CHECK(first->cacheStatus == CodeCacheStatus::Miss);
    CHECK(first->cacheNote == "no entry");
    CHECK(runNumber(std::move(first), opts) == kProgramValue);
    onlyEntry(cacheDir);

    auto second = compileScript(kProgram, opts);
    CHECK(second->cacheStatus == CodeCacheStatus::Hit);
    CHECK(runNumber(std::move(second), opts) == kProgramValue);

    // No directory: the cache is off.
    EvalOptions off = opts;
    off.codeCacheDir.clear();
    auto plain = compileScript(kProgram, off);
    CHECK(plain->cacheStatus == CodeCacheStatus::Off);
    CHECK(runNumber(std::move(plain), off) == kProgramValue);
}

TEST_CASE("code cache: any compile input that differs is a different key") {
    const auto cacheDir = makeDir("bronze_cc_key");
    const EvalOptions opts = cached(cacheDir, "<cc-key>");
    CHECK(runNumber(compileScript("6 * 7;", opts), opts) == 42.0);
    CHECK(compileScript("6 * 7;", opts)->cacheStatus == CodeCacheStatus::Hit);

    auto edited = compileScript("6 * 8;", opts);
    CHECK(edited->cacheStatus == CodeCacheStatus::Miss);
    CHECK(runNumber(std::move(edited), opts) == 48.0);

    EvalOptions globals = opts;
    globals.hostGlobals = {"console", "somethingElse"};
    CHECK(compileScript("6 * 7;", globals)->cacheStatus == CodeCacheStatus::Miss);

    EvalOptions renamed = opts;
    renamed.filename = "<cc-key-other>";
    CHECK(compileScript("6 * 7;", renamed)->cacheStatus == CodeCacheStatus::Miss);

    // The tier is not part of the key: the IL is the same at every tier.
    EvalOptions tier0 = opts;
    tier0.tier = ExecutionTier::Tier0_Interpreter;
    auto interp = compileScript("6 * 7;", tier0);
    CHECK(interp->cacheStatus == CodeCacheStatus::Hit);
    CHECK(runNumber(std::move(interp), tier0) == 42.0);
}

TEST_CASE("code cache: editing an imported module invalidates the entry") {
    const auto dir = makeDir("bronze_cc_graph");
    const auto cacheDir = dir / "cache";
    writeFile(dir / "dep.js", "export function value() { return 10; }\n");
    writeFile(dir / "main.js", "import { value } from './dep.js';\nglobalThis.__ccGraph = value() + 1;\n");
    const std::string entry = (dir / "main.js").string();
    EvalOptions opts = cached(cacheDir, entry);

    auto readResult = [] {
        embed::GlobalValue g = embed::globalValue("__ccGraph");
        REQUIRE(g.found);
        return g.value.asNumber();
    };

    auto first = compileFile(entry, opts);
    CHECK(first->cacheStatus == CodeCacheStatus::Miss);
    REQUIRE(!runCompiledScript(std::move(first), opts).thrown);
    CHECK(readResult() == 11.0);

    auto hit = compileFile(entry, opts);
    CHECK(hit->cacheStatus == CodeCacheStatus::Hit);
    REQUIRE(!runCompiledScript(std::move(hit), opts).thrown);
    CHECK(readResult() == 11.0);

    writeFile(dir / "dep.js", "export function value() { return 20; }\n");
    auto stale = compileFile(entry, opts);
    CHECK(stale->cacheStatus == CodeCacheStatus::Miss);
    CHECK(stale->cacheNote.rfind("source changed", 0) == 0);
    REQUIRE(!runCompiledScript(std::move(stale), opts).thrown);
    CHECK(readResult() == 21.0);

    // The miss replaced the entry.
    auto again = compileFile(entry, opts);
    CHECK(again->cacheStatus == CodeCacheStatus::Hit);
    REQUIRE(!runCompiledScript(std::move(again), opts).thrown);
    CHECK(readResult() == 21.0);

    // A module that is gone is a miss too, and the compile reports it.
    std::filesystem::remove(dir / "dep.js");
    auto gone = compileFile(entry, opts);
    CHECK(gone->cacheStatus == CodeCacheStatus::Miss);
    CHECK_FALSE(gone->success);
}

TEST_CASE("code cache: a new file under an import() glob invalidates the entry") {
    const auto dir = makeDir("bronze_cc_glob");
    const auto cacheDir = dir / "cache";
    std::filesystem::create_directories(dir / "plugins");
    writeFile(dir / "plugins" / "a.js", "export const n = 1;\n");
    const std::string src =
        "const name = globalThis.__ccPlugin || 'a';\n"
        "import(`./plugins/${name}.js`).then((m) => { globalThis.__ccGlob = m.n; });\n";
    EvalOptions opts = cached(cacheDir, (dir / "main.js").string());
    opts.entryResolvesAs = dir / "main.js";

    CHECK(compileScript(src, opts)->cacheStatus == CodeCacheStatus::Miss);
    CHECK(compileScript(src, opts)->cacheStatus == CodeCacheStatus::Hit);

    writeFile(dir / "plugins" / "b.js", "export const n = 2;\n");
    auto changed = compileScript(src, opts);
    CHECK(changed->cacheStatus == CodeCacheStatus::Miss);
    CHECK(changed->cacheNote == "module resolution changed");
    CHECK(compileScript(src, opts)->cacheStatus == CodeCacheStatus::Hit);
}

TEST_CASE("code cache: a damaged or truncated entry falls back to a full compile") {
    const auto cacheDir = makeDir("bronze_cc_corrupt");
    const EvalOptions opts = cached(cacheDir, "<cc-corrupt>");
    CHECK(runNumber(compileScript(kProgram, opts), opts) == kProgramValue);
    const std::filesystem::path entryPath = onlyEntry(cacheDir);
    const std::string good = readFile(entryPath);
    REQUIRE(good.size() > 100);

    struct Damage {
        const char* what;
        std::string bytes;
        const char* note;
    };
    std::string flipped = good;
    flipped[good.size() / 2] = static_cast<char>(flipped[good.size() / 2] ^ 0x5A);
    std::string badKey = good;
    badKey[20] = static_cast<char>(badKey[20] ^ 1);
    const std::vector<Damage> damages = {
        {"truncated", good.substr(0, good.size() / 2), "truncated"},
        {"header only", good.substr(0, 30), "not a cache entry"},
        {"empty", std::string(), "not a cache entry"},
        {"flipped payload byte", flipped, "checksum mismatch"},
        {"flipped key byte", badKey, "key mismatch"},
        {"extended", good + "trailing", "truncated"},
        {"garbage", std::string(4096, '\x7f'), "not a cache entry"},
    };
    for (const auto& d : damages) {
        CAPTURE(d.what);
        writeFile(entryPath, d.bytes);
        auto script = compileScript(kProgram, opts);
        CHECK(script->cacheStatus == CodeCacheStatus::Miss);
        CHECK(script->cacheNote == d.note);
        CHECK(runNumber(std::move(script), opts) == kProgramValue);
        // The miss rewrote a good entry.
        CHECK(readFile(entryPath) == good);
        CHECK(compileScript(kProgram, opts)->cacheStatus == CodeCacheStatus::Hit);
    }
}

TEST_CASE("code cache: the directory is trimmed to its byte limit") {
    const auto cacheDir = makeDir("bronze_cc_trim");
    EvalOptions opts = cached(cacheDir, "<cc-trim>");
    opts.codeCacheMaxBytes = 1;  // every store evicts all older entries
    CHECK(runNumber(compileScript("1 + 1;", opts), opts) == 2.0);
    CHECK(runNumber(compileScript("2 + 2;", opts), opts) == 4.0);
    int entries = 0;
    for (const auto& e : std::filesystem::directory_iterator(cacheDir)) entries += e.path().extension() == ".bzc";
    CHECK(entries <= 1);
}

TEST_CASE("captured inputs compile on another thread; progress reaches done") {
    const auto cacheDir = makeDir("bronze_cc_thread");
    EvalOptions opts = cached(cacheDir, "<cc-thread>");
    std::vector<std::string> phases;
    std::vector<double> fractions;
    opts.onProgress = [&](const CompileProgress& p) {
        phases.emplace_back(p.phase);
        fractions.push_back(p.fraction);
    };
    captureThreadInputs(opts);
    REQUIRE(opts.nativeManifestJson.has_value());

    std::unique_ptr<CompiledScript> script;
    std::thread worker([&] { script = compileScript(kProgram, opts); });
    worker.join();
    CHECK(runNumber(std::move(script), opts) == kProgramValue);
    REQUIRE(!phases.empty());
    CHECK(phases.front() == "load");
    CHECK(phases.back() == "done");
    for (size_t i = 1; i < fractions.size(); ++i) CHECK(fractions[i] >= fractions[i - 1]);

    phases.clear();
    fractions.clear();
    std::thread again([&] { script = compileScript(kProgram, opts); });
    again.join();
    CHECK(script->cacheStatus == CodeCacheStatus::Hit);
    CHECK(runNumber(std::move(script), opts) == kProgramValue);
    REQUIRE(phases.size() == 2);
    CHECK(phases[0] == "cache");
    CHECK(phases[1] == "done");
}
