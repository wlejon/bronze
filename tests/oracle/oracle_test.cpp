// Differential oracle harness. Each cases/<name>.js has a pinned
// cases/<name>.expected holding the exact stdout bytes of a correct run (JS
// semantics, console.log lines). bronze compiles the case and the produced
// executable's stdout must match byte-for-byte. Compiled cases run under a hard
// timeout and are killed on expiry, so a miscompiled loop can never hang the
// suite.
//
// A case that needs SEVERAL files is a directory: `cases/<name>/main.js` is the
// entry, its neighbours are what it imports, and the expectation is
// `cases/<name>/main.expected` — the same "entry path with the extension
// replaced" rule, one level deeper. Everything below this point treats the two
// kinds identically.
//
// Ratchet rules: expectations are never edited to match bronze; a
// cases/blocked/ entry that builds and matches must be promoted to cases/; and
// every case is compiled and run BOTH with inference and with `--no-infer`,
// both of which must produce the pinned bytes.
//
// The JIT half (the "Oracle JIT" test case below, its own ctest test): every
// case is ALSO run through `bronze run`, the in-process JIT path the bro
// engine uses for an app without an app.dll, and its stdout must match the
// same pinned bytes. Both paths load the same brass object, so a divergence
// lives in what surrounds it — entry and exception propagation, hook and
// host-global installation, symbol resolution — and the harness also holds
// the exit code and the uncaught-error report to be identical between the
// built program and the JIT run of the same source.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli/driver.h"
#include "run_process.h"

#ifndef TEST_CASES_DIR
#define TEST_CASES_DIR "tests/oracle/cases"
#endif

#ifndef TEST_THREEJS_DIR
#define TEST_THREEJS_DIR "tests/oracle/threejs"
#endif

#ifndef TEST_PIXI_DIR
#define TEST_PIXI_DIR "tests/oracle/pixi"
#endif

#ifndef TEST_BRONZE_CLI
#define TEST_BRONZE_CLI "bronze"
#endif

namespace {

using oracle::kRunTimeoutMs;
using oracle::RunResult;

RunResult runWithTimeout(const std::string& exePath, bool gcStress = false,
                         uint32_t timeoutMs = kRunTimeoutMs) {
    return oracle::runCommand(oracle::quoted(exePath), gcStress, timeoutMs);
}

// `bronze run <entry>`: the source compiled in-process by the JIT and run in
// that same process, as the engine does it. `hostGlobals` is the manifest a
// build of the same case would take.
RunResult runInJit(const std::filesystem::path& entry, bool gcStress = false,
                   uint32_t timeoutMs = kRunTimeoutMs, const std::string& hostGlobals = {}) {
    std::string cmd = oracle::quoted(TEST_BRONZE_CLI) + " run " + oracle::quoted(entry.string());
    if (!hostGlobals.empty()) cmd += " --host-globals " + oracle::quoted(hostGlobals);
    return oracle::runCommand(cmd, gcStress, timeoutMs);
}

// `bronze run --tier=<tier> <entry>`: the same in-process run pinned to one
// execution tier (0 fast interpreter, 1 baseline, 2 optimizing, auto tiered).
RunResult runAtTier(const std::filesystem::path& entry, const std::string& tier, bool gcStress) {
    std::string cmd = oracle::quoted(TEST_BRONZE_CLI) + " run --tier=" + tier + " " +
                      oracle::quoted(entry.string());
    return oracle::runCommand(cmd, gcStress, kRunTimeoutMs);
}

bool readFileBytes(const std::filesystem::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// A built program is the executable and the module beside it (cli/link.h);
// removing one and leaving the other is how a stale module gets run by the
// next build's host. The staged runtime stays: it is one file per directory,
// shared by every case built there.
void removeProgram(const std::filesystem::path& exe, std::error_code& ec) {
    std::filesystem::remove(exe, ec);
    std::filesystem::path module = exe;
#if defined(_WIN32)
    module.replace_extension(".dll");
#elif defined(__APPLE__)
    module.replace_extension(".dylib");
#else
    module.replace_extension(".so");
#endif
    std::filesystem::remove(module, ec);
}

std::filesystem::path findTestDirectory(const std::filesystem::path& baked,
                                        const std::string& suffix) {
    std::vector<std::filesystem::path> candidates = {
        baked,
        suffix,
        "../" + suffix,
        "../../" + suffix,
        "../../../" + suffix
    };

    std::filesystem::path cwd = std::filesystem::current_path();
    for (const auto& cand : candidates) {
        std::filesystem::path full = cwd / cand;
        std::error_code ec;
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
            return std::filesystem::canonical(cand, ec);
        }
        if (std::filesystem::exists(full, ec) && std::filesystem::is_directory(full, ec)) {
            return std::filesystem::canonical(full, ec);
        }
    }
    return {};
}

std::filesystem::path findCasesDirectory() {
    return findTestDirectory(TEST_CASES_DIR, "tests/oracle/cases");
}

struct OracleCase {
    std::filesystem::path entry;
    std::string id;
};

std::vector<OracleCase> casesIn(const std::filesystem::path& dir) {
    std::vector<OracleCase> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".js") {
            cases.push_back({entry.path(), entry.path().stem().string()});
            continue;
        }
        if (!entry.is_directory()) continue;
        if (entry.path().filename() == "blocked") continue;
        std::filesystem::path main = entry.path() / "main.js";
        std::error_code ec;
        if (!std::filesystem::exists(main, ec)) continue;
        cases.push_back({main, entry.path().filename().string()});
    }
    std::sort(cases.begin(), cases.end(),
              [](const OracleCase& a, const OracleCase& b) { return a.entry < b.entry; });
    return cases;
}

unsigned int getWorkerJobCount() {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "BRONZE_TEST_JOBS") == 0 && env != nullptr) {
        int n = std::atoi(env);
        std::free(env);
        if (n > 0) return static_cast<unsigned int>(n);
    }
#else
    if (const char* env = std::getenv("BRONZE_TEST_JOBS")) {
        int n = std::atoi(env);
        if (n > 0) return static_cast<unsigned int>(n);
    }
#endif
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    // Bounded concurrency: at most 4 threads by default (or hw / 2) to prevent machine saturation
    return std::max(1u, std::min(4u, hw / 2));
}

// Does this case READ THE CLOCK? That is the thing a pinned expectation cannot
// survive, and it is narrower than "mentions Date": `Date.UTC`, `Date.parse`,
// the field constructor and every one of 21.4.4's members are pure functions of
// their arguments, and pinning them is exactly how the Date implementation is
// held to ECMA-262.
//
// The three `Date` clock reads are `Date.now`, `new Date()` and `Date()` — the
// two no-argument constructor forms. A whitespace-tolerant match on the empty
// argument list is what separates them from `new Date(0)`.
//
// `performance.now` is the fourth, and it is a clock this suite cannot pin for
// exactly the same reason: it is monotonic and sub-millisecond, so it is a
// BETTER clock and therefore a worse thing to have in a byte-pinned
// expectation. Its own tests live in tests/cli/performance_now_test.cpp, where
// what is asserted is a relation rather than a string.
//
// The occurrence must be the IDENTIFIER `Date` and not a name that merely ends
// in it: `d.getUTCDate()` is four characters of "Date" followed by an empty
// argument list, and reading it as the constructor would ban the very getters
// these cases exist to pin.
//
// (Local-TIMEZONE dependence is a second hazard this cannot see: a case may
// call `getHours` only inside a relation that holds in every zone. That is a
// rule for the author, stated in tests/oracle/README.md.)
bool standsAlone(const std::string& code, size_t at) {
    if (at == 0) return true;
    const char before = code[at - 1];
    // '.' rejects a member access, so a `getUTCDate`/`setDate` suffix and a
    // user's own `foo.Date` are both something other than the global.
    return !(std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '$' ||
             before == '.');
}

bool readsTheClock(const std::string& code) {
    size_t perf = 0;
    while ((perf = code.find("performance.now", perf)) != std::string::npos) {
        if (standsAlone(code, perf)) return true;
        perf += 11;
    }
    size_t now = 0;
    while ((now = code.find("Date.now", now)) != std::string::npos) {
        if (standsAlone(code, now)) return true;
        now += 4;
    }
    size_t at = 0;
    while ((at = code.find("Date", at)) != std::string::npos) {
        if (!standsAlone(code, at)) {
            at += 4;
            continue;
        }
        size_t after = at + 4;
        while (after < code.size() && (code[after] == ' ' || code[after] == '\t')) ++after;
        if (after < code.size() && code[after] == '(') {
            size_t inner = after + 1;
            while (inner < code.size() &&
                   (code[inner] == ' ' || code[inner] == '\t' || code[inner] == '\n' ||
                    code[inner] == '\r')) {
                ++inner;
            }
            if (inner < code.size() && code[inner] == ')') return true;
        }
        at = after;
    }
    return false;
}

struct CaseExecutionResult {
    OracleCase oracleCase;
    bool codeReadOk = false;
    bool hasNonDeterminism = false;
    bool expectedReadOk = false;
    std::string expectedPathStr;
    std::string expected;

    // Inference mode (normal + gc stress on the same built binary)
    int buildInferStatus = -1;
    std::string buildInferErr;
    bool inferExeExists = false;
    RunResult runInfer;
    RunResult runInferGc;

    // No-inference mode
    int buildNoInferStatus = -1;
    std::string buildNoInferErr;
    bool noInferExeExists = false;
    RunResult runNoInfer;
    RunResult runNoInferGc;
};

}  // namespace

TEST_CASE("Oracle differential test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    auto caseFiles = casesIn(casesDir);
    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    static std::vector<CaseExecutionResult> results;
    static std::once_flag resultsOnce;

    std::call_once(resultsOnce, [&] {
        results.resize(caseFiles.size());
        const unsigned int numJobs = getWorkerJobCount();
        std::atomic<size_t> nextCaseIdx{0};

        auto worker = [&] {
            while (true) {
                size_t idx = nextCaseIdx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= caseFiles.size()) break;

                const auto& oracleCase = caseFiles[idx];
                CaseExecutionResult& res = results[idx];
                res.oracleCase = oracleCase;

                std::string code;
                res.codeReadOk = readFileBytes(oracleCase.entry, code);
                if (res.codeReadOk) {
                    res.hasNonDeterminism =
                        readsTheClock(code) || code.find("Math.random") != std::string::npos;
                }

                std::filesystem::path expectedPath = oracleCase.entry;
                expectedPath.replace_extension(".expected");
                res.expectedPathStr = expectedPath.string();
                res.expectedReadOk = readFileBytes(expectedPath, res.expected);

                if (!res.codeReadOk || !res.expectedReadOk) continue;

                // 1. Compile with inference on
                std::filesystem::path exeInfer =
                    std::filesystem::temp_directory_path() / (oracleCase.id + "_oracle.exe");
                std::error_code ec;
                removeProgram(exeInfer, ec);

                res.buildInferStatus = bronze::cli::runBuild(oracleCase.entry.string(),
                                                             exeInfer.string(), &res.buildInferErr, true);
                res.inferExeExists = std::filesystem::exists(exeInfer);

                if (res.buildInferStatus == 0 && res.inferExeExists) {
                    res.runInfer = runWithTimeout(exeInfer.string(), /*gcStress=*/false);
                    // Same compiled binary re-run under GC stress to verify rooting without duplicate builds
                    res.runInferGc = runWithTimeout(exeInfer.string(), /*gcStress=*/true);
                }
                removeProgram(exeInfer, ec);

                // 2. Compile with --no-infer
                std::filesystem::path exeNoInfer =
                    std::filesystem::temp_directory_path() / (oracleCase.id + "_oracle_noinfer.exe");
                removeProgram(exeNoInfer, ec);

                res.buildNoInferStatus = bronze::cli::runBuild(
                    oracleCase.entry.string(), exeNoInfer.string(), &res.buildNoInferErr, false);
                res.noInferExeExists = std::filesystem::exists(exeNoInfer);

                if (res.buildNoInferStatus == 0 && res.noInferExeExists) {
                    res.runNoInfer = runWithTimeout(exeNoInfer.string(), /*gcStress=*/false);
                    // Same compiled binary re-run under GC stress to verify rooting without duplicate builds
                    res.runNoInferGc = runWithTimeout(exeNoInfer.string(), /*gcStress=*/true);
                }
                removeProgram(exeNoInfer, ec);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(numJobs);
        for (unsigned int i = 0; i < numJobs; ++i) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            t.join();
        }
    });

    // Report results sequentially to doctest
    for (const auto& res : results) {
        SUBCASE(res.oracleCase.id.c_str()) {
            REQUIRE(res.codeReadOk);
            CHECK_MESSAGE(!res.hasNonDeterminism,
                          "Banned non-determinism (a clock read or Math.random) in this case");
            REQUIRE_MESSAGE(res.expectedReadOk,
                            ("Missing pinned expectation " + res.expectedPathStr).c_str());

            // Inference on
            std::string inferBuildMsg = "Bronze build failed for " + res.oracleCase.entry.string() +
                                        " (inference on): " + res.buildInferErr;
            INFO(inferBuildMsg);
            REQUIRE(res.buildInferStatus == 0);
            REQUIRE(res.inferExeExists);
            CHECK_MESSAGE(!res.runInfer.timedOut,
                          ("Compiled case did not finish within the timeout: " +
                           res.oracleCase.entry.string() + " (inference on)").c_str());
            if (res.runInfer.ran) {
                CHECK_MESSAGE(res.expected == res.runInfer.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (inference on)").c_str());
            }

            // GC stress (inference on)
            CHECK_MESSAGE(!res.runInferGc.timedOut,
                          ("Compiled case did not finish within the timeout (gc-stress): " +
                           res.oracleCase.entry.string()).c_str());
            if (res.runInferGc.ran) {
                CHECK_MESSAGE(res.expected == res.runInferGc.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (gc-stress)").c_str());
            }

            // No-inference mode
            std::string noInferBuildMsg = "Bronze build failed for " + res.oracleCase.entry.string() +
                                          " (--no-infer): " + res.buildNoInferErr;
            INFO(noInferBuildMsg);
            REQUIRE(res.buildNoInferStatus == 0);
            REQUIRE(res.noInferExeExists);
            CHECK_MESSAGE(!res.runNoInfer.timedOut,
                          ("Compiled case did not finish within the timeout: " +
                           res.oracleCase.entry.string() + " (--no-infer)").c_str());
            if (res.runNoInfer.ran) {
                CHECK_MESSAGE(res.expected == res.runNoInfer.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (--no-infer)").c_str());
            }

            // GC stress (--no-infer)
            CHECK_MESSAGE(!res.runNoInferGc.timedOut,
                          ("Compiled case did not finish within the timeout (gc-stress, --no-infer): " +
                           res.oracleCase.entry.string()).c_str());
            if (res.runNoInferGc.ran) {
                CHECK_MESSAGE(res.expected == res.runNoInferGc.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (gc-stress, --no-infer)").c_str());
            }
        }
    }
}

namespace {

// One case through the JIT, beside the built program it is held against.
struct JitCaseResult {
    OracleCase oracleCase;
    bool expectedReadOk = false;
    std::string expectedPathStr;
    std::string expected;

    // The reference: the same source built (inference on) and run once. Its
    // exit code and stderr are what the JIT run must reproduce.
    int buildStatus = -1;
    std::string buildErr;
    bool exeExists = false;
    RunResult runBuilt;

    RunResult runJit;
    RunResult runJitGc;
};

// The uncaught-error report both paths print at the end: the built program
// through bronze_uncaught_exception, `bronze run` through cli/run.cpp's
// reportUncaught, both from rtUncaughtReport. Held to be the same bytes.
void checkJitMatchesBuilt(const JitCaseResult& res) {
    const std::string& id = res.oracleCase.id;
    CHECK_MESSAGE(!res.runJit.timedOut,
                  ("bronze run did not finish within the timeout: " + res.oracleCase.entry.string()).c_str());
    if (res.runJit.ran) {
        CHECK_MESSAGE(res.expected == res.runJit.output,
                      ("JIT output differs from the pinned expectation for " + id).c_str());
        if (res.runBuilt.ran) {
            CHECK_MESSAGE(res.runBuilt.exitCode == res.runJit.exitCode,
                          ("JIT exit code " + std::to_string(res.runJit.exitCode) +
                           " differs from the built program's " + std::to_string(res.runBuilt.exitCode) +
                           " for " + id)
                              .c_str());
            CHECK_MESSAGE(res.runBuilt.errors == res.runJit.errors,
                          ("JIT stderr differs from the built program's for " + id + "\n--- built ---\n" +
                           res.runBuilt.errors + "\n--- jit ---\n" + res.runJit.errors)
                              .c_str());
        }
    }
    CHECK_MESSAGE(!res.runJitGc.timedOut,
                  ("bronze run did not finish within the timeout (gc-stress): " +
                   res.oracleCase.entry.string()).c_str());
    if (res.runJitGc.ran) {
        CHECK_MESSAGE(res.expected == res.runJitGc.output,
                      ("JIT output differs from the pinned expectation for " + id + " (gc-stress)").c_str());
        if (res.runBuilt.ran) {
            CHECK_MESSAGE(res.runBuilt.exitCode == res.runJitGc.exitCode,
                          ("JIT exit code differs from the built program's for " + id + " (gc-stress)").c_str());
        }
    }
}

}  // namespace

TEST_CASE("Oracle JIT differential test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    REQUIRE_MESSAGE(std::filesystem::exists(TEST_BRONZE_CLI),
                    "bronze CLI not found at " TEST_BRONZE_CLI);

    auto caseFiles = casesIn(casesDir);
    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    static std::vector<JitCaseResult> results;
    static std::once_flag resultsOnce;

    std::call_once(resultsOnce, [&] {
        results.resize(caseFiles.size());
        const unsigned int numJobs = getWorkerJobCount();
        std::atomic<size_t> nextCaseIdx{0};

        auto worker = [&] {
            while (true) {
                size_t idx = nextCaseIdx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= caseFiles.size()) break;

                const auto& oracleCase = caseFiles[idx];
                JitCaseResult& res = results[idx];
                res.oracleCase = oracleCase;

                std::filesystem::path expectedPath = oracleCase.entry;
                expectedPath.replace_extension(".expected");
                res.expectedPathStr = expectedPath.string();
                res.expectedReadOk = readFileBytes(expectedPath, res.expected);
                if (!res.expectedReadOk) continue;

                // Its own output name: the AOT suite may be building the same
                // case in another ctest process at the same time.
                std::filesystem::path exe =
                    std::filesystem::temp_directory_path() / (oracleCase.id + "_oracle_jitref.exe");
                std::error_code ec;
                removeProgram(exe, ec);
                res.buildStatus =
                    bronze::cli::runBuild(oracleCase.entry.string(), exe.string(), &res.buildErr, true);
                res.exeExists = std::filesystem::exists(exe);
                if (res.buildStatus == 0 && res.exeExists) {
                    res.runBuilt = runWithTimeout(exe.string(), /*gcStress=*/false);
                }
                removeProgram(exe, ec);

                res.runJit = runInJit(oracleCase.entry, /*gcStress=*/false);
                res.runJitGc = runInJit(oracleCase.entry, /*gcStress=*/true);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(numJobs);
        for (unsigned int i = 0; i < numJobs; ++i) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            t.join();
        }
    });

    for (const auto& res : results) {
        SUBCASE(res.oracleCase.id.c_str()) {
            REQUIRE_MESSAGE(res.expectedReadOk,
                            ("Missing pinned expectation " + res.expectedPathStr).c_str());
            std::string buildMsg =
                "Bronze build failed for " + res.oracleCase.entry.string() + ": " + res.buildErr;
            INFO(buildMsg);
            REQUIRE(res.buildStatus == 0);
            REQUIRE(res.exeExists);
            CHECK_MESSAGE(!res.runBuilt.timedOut,
                          ("Compiled case did not finish within the timeout: " +
                           res.oracleCase.entry.string()).c_str());
            checkJitMatchesBuilt(res);
        }
    }
}

// The tiers_* cases (OSR, generators, async, try/finally, GC churn) run at each
// execution tier on its own, then tiered, plain and under gc-stress: every
// tier's lowering of the same program is held to the same pinned bytes.
TEST_CASE("Oracle tiers test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    REQUIRE_MESSAGE(std::filesystem::exists(TEST_BRONZE_CLI),
                    "bronze CLI not found at " TEST_BRONZE_CLI);

    std::vector<OracleCase> caseFiles;
    for (auto& c : casesIn(casesDir)) {
        if (c.id.rfind("tiers_", 0) == 0) caseFiles.push_back(std::move(c));
    }
    REQUIRE_MESSAGE(!caseFiles.empty(), "No tiers_* cases found in cases directory");

    static const std::array<std::string, 4> kTiers = {"0", "1", "2", "auto"};
    struct TierRuns {
        std::string expected;
        bool expectedReadOk = false;
        std::array<RunResult, 4> plain;
        std::array<RunResult, 4> gc;
    };
    static std::vector<TierRuns> results;
    static std::once_flag resultsOnce;

    std::call_once(resultsOnce, [&] {
        results.resize(caseFiles.size());
        const unsigned int numJobs = getWorkerJobCount();
        std::atomic<size_t> nextCaseIdx{0};

        auto worker = [&] {
            while (true) {
                size_t idx = nextCaseIdx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= caseFiles.size()) break;
                TierRuns& res = results[idx];
                std::filesystem::path expectedPath = caseFiles[idx].entry;
                expectedPath.replace_extension(".expected");
                res.expectedReadOk = readFileBytes(expectedPath, res.expected);
                if (!res.expectedReadOk) continue;
                for (size_t t = 0; t < kTiers.size(); ++t) {
                    res.plain[t] = runAtTier(caseFiles[idx].entry, kTiers[t], /*gcStress=*/false);
                    res.gc[t] = runAtTier(caseFiles[idx].entry, kTiers[t], /*gcStress=*/true);
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(numJobs);
        for (unsigned int i = 0; i < numJobs; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    });

    for (size_t i = 0; i < caseFiles.size(); ++i) {
        const std::string& id = caseFiles[i].id;
        const TierRuns& res = results[i];
        SUBCASE(id.c_str()) {
            REQUIRE_MESSAGE(res.expectedReadOk, ("Missing pinned expectation for " + id).c_str());
            for (size_t t = 0; t < kTiers.size(); ++t) {
                const std::string where = id + " at --tier=" + kTiers[t];
                CHECK_MESSAGE(!res.plain[t].timedOut, ("timed out: " + where).c_str());
                CHECK_MESSAGE(res.plain[t].exitCode == 0,
                              ("exit " + std::to_string(res.plain[t].exitCode) + ": " + where +
                               "\n" + res.plain[t].errors).c_str());
                CHECK_MESSAGE(res.expected == res.plain[t].output,
                              ("output differs from the pinned expectation: " + where).c_str());
                CHECK_MESSAGE(!res.gc[t].timedOut, ("timed out (gc-stress): " + where).c_str());
                CHECK_MESSAGE(res.expected == res.gc[t].output,
                              ("output differs from the pinned expectation (gc-stress): " + where +
                               "\n" + res.gc[t].errors).c_str());
            }
        }
    }
}

TEST_CASE("Oracle blocked test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    std::filesystem::path blockedDir = casesDir / "blocked";
    std::error_code ec;
    if (!std::filesystem::exists(blockedDir, ec) || !std::filesystem::is_directory(blockedDir, ec)) {
        return;
    }

    for (const auto& oracleCase : casesIn(blockedDir)) {
        const std::filesystem::path& casePath = oracleCase.entry;
        SUBCASE(oracleCase.id.c_str()) {
            std::string code;
            REQUIRE(readFileBytes(casePath, code));
            CHECK(!readsTheClock(code));
            CHECK(code.find("Math.random") == std::string::npos);

            std::filesystem::path expectedPath = casePath;
            expectedPath.replace_extension(".expected");
            std::string expected;
            REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                            ("Missing pinned expectation " + expectedPath.string()).c_str());

            std::filesystem::path exePath =
                std::filesystem::temp_directory_path() / (oracleCase.id + "_blocked.exe");
            removeProgram(exePath, ec);

            std::string errOut;
            int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut);

            if (status == 0 && std::filesystem::exists(exePath)) {
                RunResult run = runWithTimeout(exePath.string());
                removeProgram(exePath, ec);
                bool matches = run.ran && (expected == run.output);
                CHECK_MESSAGE(!matches,
                              ("Blocked oracle case passes! Promote " + oracleCase.id +
                               " (and its .expected) from cases/blocked/ to cases/").c_str());
            } else {
                CHECK(true);  // still blocked at build time
            }
        }
    }
}

TEST_CASE("threejs milestone: unmodified r160 compiles and its scene graph holds") {
    std::filesystem::path dir = findTestDirectory(TEST_THREEJS_DIR, "tests/oracle/threejs");
    REQUIRE_MESSAGE(!dir.empty(), "tests/oracle/threejs not found");

    std::filesystem::path casePath = dir / "main.js";
    REQUIRE(std::filesystem::exists(casePath));

    std::string expected;
    std::filesystem::path expectedPath = dir / "main.expected";
    REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                    ("Missing pinned expectation " + expectedPath.string()).c_str());

    for (const bool infer : {true, false}) {
        const std::string mode = infer ? " (inference on)" : " (--no-infer)";
        std::filesystem::path exePath = std::filesystem::temp_directory_path() /
                                        (infer ? "threejs_oracle.exe" : "threejs_oracle_ni.exe");
        std::error_code ec;
        removeProgram(exePath, ec);

        std::string errOut;
        int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut, infer);
        std::string threeMsg = "Bronze failed to build three.js" + mode + ": " + errOut;
        INFO(threeMsg);
        REQUIRE(status == 0);
        REQUIRE(std::filesystem::exists(exePath));

        RunResult run = runWithTimeout(exePath.string(), /*gcStress=*/false);
        CHECK_MESSAGE(!run.timedOut, ("three.js case did not finish within the timeout" + mode).c_str());
        if (run.ran) {
            CHECK_MESSAGE(run.exitCode == 0,
                          ("three.js exited with code " + std::to_string(run.exitCode) + mode).c_str());
            CHECK_MESSAGE(expected == run.output,
                          ("three.js output differs from the pinned expectation" + mode).c_str());
        }

        // Same executable, every allocation now moving the whole live set.
        RunResult stressed = runWithTimeout(exePath.string(), /*gcStress=*/true);
        CHECK_MESSAGE(!stressed.timedOut,
                      ("three.js case did not finish within the timeout (gc-stress" + mode + ")").c_str());
        if (stressed.ran) {
            CHECK_MESSAGE(stressed.exitCode == 0,
                          ("three.js exited with code " + std::to_string(stressed.exitCode) +
                           " (gc-stress" + mode + ")").c_str());
            CHECK_MESSAGE(expected == stressed.output,
                          ("three.js output differs from the pinned expectation (gc-stress" + mode + ")").c_str());
        }
        removeProgram(exePath, ec);
    }
}

// The pixi milestone: the published pixi.js v8 ESM bundle, vendored
// byte-for-byte (tests/oracle/pixi/README.md), compiles and a scene graph
// built from its public API matches an expectation derived by reading pixi's
// source — never by running bronze or node. Same shape as the three.js
// milestone above, plus one seam that milestone does not need: pixi's
// import-time code reads two browser globals (`navigator`, `Intl`), which
// setup.mjs defines on `globalThis` and host.globals admits at compile time.
TEST_CASE("pixi milestone: unmodified v8.19.0 compiles and its scene graph holds") {
    std::filesystem::path dir = findTestDirectory(TEST_PIXI_DIR, "tests/oracle/pixi");
    REQUIRE_MESSAGE(!dir.empty(), "tests/oracle/pixi not found");

    std::filesystem::path casePath = dir / "main.js";
    REQUIRE(std::filesystem::exists(casePath));

    std::string expected;
    std::filesystem::path expectedPath = dir / "main.expected";
    REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                    ("Missing pinned expectation " + expectedPath.string()).c_str());

    const std::string hostGlobals = (dir / "host.globals").string();
    REQUIRE(std::filesystem::exists(hostGlobals));

    for (const bool infer : {true, false}) {
        const std::string mode = infer ? " (inference on)" : " (--no-infer)";
        std::filesystem::path exePath = std::filesystem::temp_directory_path() /
                                        (infer ? "pixi_oracle.exe" : "pixi_oracle_ni.exe");
        std::error_code ec;
        removeProgram(exePath, ec);

        std::string errOut;
        int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut, infer,
                                           /*timings=*/false, /*emitObj=*/false, hostGlobals);
        REQUIRE_MESSAGE(status == 0,
                        ("Bronze failed to build pixi" + mode + ": " + errOut).c_str());
        REQUIRE(std::filesystem::exists(exePath));

        RunResult run = runWithTimeout(exePath.string(), /*gcStress=*/false);
        CHECK_MESSAGE(!run.timedOut, ("pixi case did not finish within the timeout" + mode).c_str());
        if (run.ran) {
            CHECK_MESSAGE(run.exitCode == 0,
                          ("pixi exited with code " + std::to_string(run.exitCode) + mode).c_str());
            CHECK_MESSAGE(expected == run.output,
                          ("pixi output differs from the pinned expectation" + mode).c_str());
        }

        // Same executable, every allocation now moving the whole live set. A
        // whole-library import under that regime measures in minutes, not the
        // 15 s a case gets — the budget is sized to the run, not the run
        // trimmed to the budget (the byte-compare is unchanged either way).
        RunResult stressed =
            runWithTimeout(exePath.string(), /*gcStress=*/true, /*timeoutMs=*/300000);
        CHECK_MESSAGE(!stressed.timedOut,
                      ("pixi case did not finish within the timeout (gc-stress" + mode + ")").c_str());
        if (stressed.ran) {
            CHECK_MESSAGE(stressed.exitCode == 0,
                          ("pixi exited with code " + std::to_string(stressed.exitCode) +
                           " (gc-stress" + mode + ")").c_str());
            CHECK_MESSAGE(expected == stressed.output,
                          ("pixi output differs from the pinned expectation (gc-stress" + mode + ")").c_str());
        }
        removeProgram(exePath, ec);
    }
}

namespace {

// A milestone library through `bronze run`: the whole bundle compiled by the
// JIT in one process and run there, held to the same expectation the built
// program is. Both budgets are the built program's run budget PLUS the
// compile, because under `bronze run` the whole-library compile happens
// inside the timed process. The compile is the larger half for a big bundle
// (pixi: ~60 s of a ~61 s run on a fast desktop, over 120 s on a CI runner
// sharing its cores with three other suites), and it moves with the backend's
// optimizer, so it gets its own allowance rather than hiding in a multiple of
// the run budget until the optimizer grows past it.
void checkJitMilestone(const char* name, const std::filesystem::path& entry,
                       const std::string& expected, const std::string& hostGlobals,
                       uint32_t runTimeoutMs, uint32_t gcStressRunTimeoutMs,
                       uint32_t compileAllowanceMs) {
    const std::string what = std::string(name) + " (jit)";
    RunResult run = runInJit(entry, /*gcStress=*/false, runTimeoutMs + compileAllowanceMs, hostGlobals);
    CHECK_MESSAGE(!run.timedOut, (what + " did not finish within the timeout").c_str());
    if (run.ran) {
        CHECK_MESSAGE(run.exitCode == 0,
                      (what + " exited with code " + std::to_string(run.exitCode) + "\n" + run.errors).c_str());
        CHECK_MESSAGE(expected == run.output, (what + " output differs from the pinned expectation").c_str());
    }

    RunResult stressed =
        runInJit(entry, /*gcStress=*/true, gcStressRunTimeoutMs + compileAllowanceMs, hostGlobals);
    CHECK_MESSAGE(!stressed.timedOut, (what + " did not finish within the timeout (gc-stress)").c_str());
    if (stressed.ran) {
        CHECK_MESSAGE(stressed.exitCode == 0,
                      (what + " exited with code " + std::to_string(stressed.exitCode) + " (gc-stress)\n" +
                       stressed.errors).c_str());
        CHECK_MESSAGE(expected == stressed.output,
                      (what + " output differs from the pinned expectation (gc-stress)").c_str());
    }
}

}  // namespace

TEST_CASE("threejs-jit milestone: unmodified r160 runs under bronze run") {
    std::filesystem::path dir = findTestDirectory(TEST_THREEJS_DIR, "tests/oracle/threejs");
    REQUIRE_MESSAGE(!dir.empty(), "tests/oracle/threejs not found");
    REQUIRE(std::filesystem::exists(dir / "main.js"));
    std::string expected;
    REQUIRE_MESSAGE(readFileBytes(dir / "main.expected", expected),
                    ("Missing pinned expectation " + (dir / "main.expected").string()).c_str());
    // Run budgets as the built program's (runWithTimeout's default for both);
    // the compile allowance is the old 4x budget less the run it contained.
    checkJitMilestone("three.js", dir / "main.js", expected, {}, kRunTimeoutMs, kRunTimeoutMs,
                      /*compileAllowanceMs=*/kRunTimeoutMs * 3);
}

TEST_CASE("pixi-jit milestone: unmodified v8.19.0 runs under bronze run") {
    std::filesystem::path dir = findTestDirectory(TEST_PIXI_DIR, "tests/oracle/pixi");
    REQUIRE_MESSAGE(!dir.empty(), "tests/oracle/pixi not found");
    REQUIRE(std::filesystem::exists(dir / "main.js"));
    std::string expected;
    REQUIRE_MESSAGE(readFileBytes(dir / "main.expected", expected),
                    ("Missing pinned expectation " + (dir / "main.expected").string()).c_str());
    const std::string hostGlobals = (dir / "host.globals").string();
    REQUIRE(std::filesystem::exists(hostGlobals));
    // The built program's budgets (the pixi milestone above: the default run
    // budget, 300 s under gc-stress) plus a whole-pixi compile allowance.
    checkJitMilestone("pixi", dir / "main.js", expected, hostGlobals, kRunTimeoutMs,
                      /*gcStressRunTimeoutMs=*/300000, /*compileAllowanceMs=*/300000);
}
